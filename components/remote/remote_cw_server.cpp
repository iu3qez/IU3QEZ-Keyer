/**
 * @file remote_cw_server.cpp
 * @brief CWNet server implementation (partial - MORSE + PTT only)
 */

#include "remote/remote_cw_server.hpp"
#include "esp_log.h"
#include "hal/high_precision_clock.hpp"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include <cstring>

namespace remote {

namespace {
constexpr char kLogTag[] = "remote_server";

inline int64_t MillisecondsToMicroseconds(uint32_t ms) {
  return static_cast<int64_t>(ms) * 1000;
}

inline uint32_t MicrosecondsToMilliseconds(int64_t us) {
  return static_cast<uint32_t>(us / 1000);
}

}  // namespace

RemoteCwServer::RemoteCwServer()
    : config_(),
      callbacks_(),
      state_(RemoteCwServerState::kIdle),
      listen_fd_(-1),
      client_fd_(-1),
      state_enter_time_us_(0),
      rx_bytes_(0),
      tx_head_(0),
      tx_tail_(0),
      ptt_active_(false),
      ptt_timeout_us_(0),
      last_key_timestamp_us_(0) {
  std::memset(rx_buffer_, 0, sizeof(rx_buffer_));
  std::memset(tx_buffer_, 0, sizeof(tx_buffer_));
}

RemoteCwServer::~RemoteCwServer() {
  Stop();
}

void RemoteCwServer::Configure(const RemoteCwServerConfig& config,
                                const RemoteCwServerCallbacks& callbacks) {
  config_ = config;
  callbacks_ = callbacks;
}

esp_err_t RemoteCwServer::Start() {
  if (state_ != RemoteCwServerState::kIdle) {
    ESP_LOGW(kLogTag, "Server already started (state=%d)", static_cast<int>(state_));
    return ESP_ERR_INVALID_STATE;
  }

  // Create TCP listen socket
  listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd_ < 0) {
    ESP_LOGE(kLogTag, "Failed to create listen socket: errno %d", errno);
    return ESP_FAIL;
  }

  // Set SO_REUSEADDR to allow quick restart
  int reuse = 1;
  if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    ESP_LOGW(kLogTag, "Failed to set SO_REUSEADDR: errno %d", errno);
  }

  // Set non-blocking
  int flags = fcntl(listen_fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ESP_LOGE(kLogTag, "Failed to set listen socket non-blocking: errno %d", errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return ESP_FAIL;
  }

  // Bind to port
  struct sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(config_.listen_port);

  if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&server_addr),
           sizeof(server_addr)) < 0) {
    ESP_LOGE(kLogTag, "Failed to bind to port %u: errno %d", config_.listen_port, errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return ESP_FAIL;
  }

  // Start listening (backlog = 1, single client only)
  if (listen(listen_fd_, 1) < 0) {
    ESP_LOGE(kLogTag, "Failed to listen: errno %d", errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return ESP_FAIL;
  }

  ESP_LOGI(kLogTag, "Server listening on port %u", config_.listen_port);
  ChangeState(RemoteCwServerState::kListening);
  return ESP_OK;
}

void RemoteCwServer::Stop() {
  CloseClientSocket();
  CloseListenSocket();
  ptt_active_ = false;
  ptt_timeout_us_ = 0;
  ChangeState(RemoteCwServerState::kIdle);
}

void RemoteCwServer::Tick(int64_t now_us) {
  switch (state_) {
    case RemoteCwServerState::kIdle:
      // Nothing to do
      break;

    case RemoteCwServerState::kListening:
      // Check for new connection
      HandleNewConnection();
      break;

    case RemoteCwServerState::kHandshake:
    case RemoteCwServerState::kConnected:
      // Handle client I/O
      HandleSocketIo(now_us);
      // Tick PTT management
      TickRemotePtt(now_us);
      break;

    case RemoteCwServerState::kError:
      // Auto-restart if enabled
      if (config_.auto_restart) {
        ESP_LOGI(kLogTag, "Auto-restarting server...");
        Stop();
        Start();
      }
      break;
  }
}

void RemoteCwServer::ChangeState(RemoteCwServerState new_state) {
  if (state_ != new_state) {
    state_ = new_state;
    state_enter_time_us_ = hal::HighPrecisionClock::NowMicros();
    if (callbacks_.on_state_changed) {
      callbacks_.on_state_changed(new_state, callbacks_.context);
    }
  }
}

void RemoteCwServer::HandleNewConnection() {
  struct sockaddr_in client_addr{};
  socklen_t addr_len = sizeof(client_addr);

  int new_fd = accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
  if (new_fd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ESP_LOGE(kLogTag, "accept() failed: errno %d", errno);
      ChangeState(RemoteCwServerState::kError);
    }
    return;
  }

  // Got a connection!
  char client_ip[16];
  inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
  ESP_LOGI(kLogTag, "Client connected from %s:%u", client_ip, ntohs(client_addr.sin_port));

  // Set client socket non-blocking
  int flags = fcntl(new_fd, F_GETFL, 0);
  if (flags < 0 || fcntl(new_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    ESP_LOGW(kLogTag, "Failed to set client socket non-blocking: errno %d", errno);
    close(new_fd);
    return;
  }

  // Set TCP_NODELAY for low latency
  int nodelay = 1;
  if (setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
    ESP_LOGW(kLogTag, "Failed to set TCP_NODELAY: errno %d", errno);
  }

  client_fd_ = new_fd;
  std::strncpy(client_ip_, client_ip, sizeof(client_ip_) - 1);
  client_ip_[sizeof(client_ip_) - 1] = '\0';
  rx_bytes_ = 0;
  tx_head_ = 0;
  tx_tail_ = 0;

  ChangeState(RemoteCwServerState::kHandshake);
}

void RemoteCwServer::HandleSocketIo(int64_t now_us) {
  if (client_fd_ < 0) {
    return;
  }

  // Read from socket
  if (rx_bytes_ < kRxBufferCapacity) {
    ssize_t received = recv(client_fd_, rx_buffer_ + rx_bytes_,
                           kRxBufferCapacity - rx_bytes_, 0);
    if (received > 0) {
      rx_bytes_ += received;
    } else if (received == 0) {
      // Connection closed by client
      ESP_LOGI(kLogTag, "Client disconnected");
      CloseClientSocket();
      if (config_.auto_restart) {
        ChangeState(RemoteCwServerState::kListening);
      } else {
        ChangeState(RemoteCwServerState::kIdle);
      }
      return;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ESP_LOGE(kLogTag, "recv() error: errno %d", errno);
      CloseClientSocket();
      ChangeState(RemoteCwServerState::kError);
      return;
    }
  }

  // Parse incoming frames
  ParseIncomingFrames(now_us);

  // Drain TX buffer
  DrainTxBuffer();
}

void RemoteCwServer::DrainTxBuffer() {
  if (client_fd_ < 0 || tx_head_ == tx_tail_) {
    return;
  }

  size_t to_send;
  if (tx_tail_ > tx_head_) {
    to_send = tx_tail_ - tx_head_;
  } else {
    to_send = kTxBufferCapacity - tx_head_;
  }

  ssize_t sent = send(client_fd_, tx_buffer_ + tx_head_, to_send, 0);
  if (sent > 0) {
    tx_head_ = (tx_head_ + sent) % kTxBufferCapacity;
  } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    ESP_LOGE(kLogTag, "send() error: errno %d", errno);
    CloseClientSocket();
    ChangeState(RemoteCwServerState::kError);
  }
}

void RemoteCwServer::ParseIncomingFrames(int64_t now_us) {
  while (rx_bytes_ > 0) {
    size_t frame_size, payload_offset, payload_size;
    uint8_t command;

    if (!TryExtractFrame(&frame_size, &command, &payload_offset, &payload_size)) {
      break;  // Need more data
    }

    const uint8_t* payload = (payload_size > 0) ? (rx_buffer_ + payload_offset) : nullptr;
    HandleFrame(command, payload, payload_size, now_us);

    // Remove processed frame from buffer
    if (frame_size < rx_bytes_) {
      std::memmove(rx_buffer_, rx_buffer_ + frame_size, rx_bytes_ - frame_size);
    }
    rx_bytes_ -= frame_size;
  }
}

bool RemoteCwServer::TryExtractFrame(size_t* frame_size, uint8_t* command,
                                     size_t* payload_offset, size_t* payload_size) const {
  if (rx_bytes_ == 0) {
    return false;
  }

  const uint8_t raw_command = rx_buffer_[0];
  *command = raw_command;

  const uint8_t block_len = (raw_command & kCmdMaskBlockLen) >> 6;
  const FrameCategory category = static_cast<FrameCategory>(block_len);

  size_t payload_len = 0;
  size_t offset = 1;
  size_t required = 1;

  switch (category) {
    case FrameCategory::kNoPayload:
      required = 1;
      offset = 1;
      break;
    case FrameCategory::kShortPayload:
      if (rx_bytes_ < 2) {
        return false;
      }
      payload_len = rx_buffer_[1];
      required = 2 + payload_len;
      offset = 2;
      break;
    case FrameCategory::kLongPayload:
      if (rx_bytes_ < 3) {
        return false;
      }
      payload_len = rx_buffer_[1] | (static_cast<size_t>(rx_buffer_[2]) << 8);
      required = 3 + payload_len;
      offset = 3;
      break;
    case FrameCategory::kReserved:
      ESP_LOGW(kLogTag, "Reserved frame encoding 0x%02X", raw_command);
      return false;
  }

  if (rx_bytes_ < required) {
    return false;
  }

  *frame_size = required;
  *payload_offset = offset;
  *payload_size = payload_len;
  return true;
}

void RemoteCwServer::HandleFrame(uint8_t command, const uint8_t* payload,
                                  size_t payload_size, int64_t now_us) {
  switch (command) {
    case kCmdConnect:
      HandleConnectFrame(payload, payload_size);
      break;
    case kCmdPing:
      HandlePingFrame(payload, payload_size);
      break;
    case kCmdMorse:
      HandleMorseFrame(payload, payload_size, now_us);
      break;
    default:
      ESP_LOGD(kLogTag, "Unhandled command 0x%02X (stub)", command);
      break;
  }
}

void RemoteCwServer::HandleConnectFrame(const uint8_t* payload, size_t payload_size) {
  if (payload_size < sizeof(ConnectPayload)) {
    ESP_LOGW(kLogTag, "CONNECT payload too small (%u)", static_cast<unsigned>(payload_size));
    return;
  }

  const ConnectPayload* connect = reinterpret_cast<const ConnectPayload*>(payload);
  ESP_LOGI(kLogTag, "Client CONNECT: username='%.*s', callsign='%.*s'",
           (int)sizeof(connect->username), connect->username,
           (int)sizeof(connect->callsign), connect->callsign);

  // Send CONNECT ACK
  SendConnectAck();

  ChangeState(RemoteCwServerState::kConnected);
}

void RemoteCwServer::HandlePingFrame(const uint8_t* payload, size_t payload_size) {
  if (payload_size != 16) {
    ESP_LOGW(kLogTag, "Invalid PING payload size: %u", static_cast<unsigned>(payload_size));
    return;
  }

  const uint8_t type = payload[0];
  const uint8_t sequence = payload[1];

  if (type == 0) {
    // Client sent REQUEST, respond with type 1
    const uint32_t now_ms = MicrosecondsToMilliseconds(hal::HighPrecisionClock::NowMicros());
    SendPingResponse(1, sequence, now_ms);
  } else if (type == 2) {
    // Client sent final ack, respond with type 2 (echo)
    SendPingResponse(2, sequence, 0);
  }
}

void RemoteCwServer::HandleMorseFrame(const uint8_t* payload, size_t payload_size,
                                      int64_t now_us) {
  if (!callbacks_.on_key_event) {
    return;
  }

  // Process each CW byte in the frame
  for (size_t i = 0; i < payload_size; ++i) {
    const uint8_t cw_byte = payload[i];
    const bool key_down = (cw_byte & 0x80) != 0;
    const uint8_t ts_encoded = cw_byte & 0x7F;
    const uint32_t delta_ms = DecodeTimestamp(ts_encoded);
    const int64_t delta_us = MillisecondsToMicroseconds(delta_ms);

    if (last_key_timestamp_us_ == 0) {
      last_key_timestamp_us_ = now_us;
    } else {
      last_key_timestamp_us_ += delta_us;
    }

    // Notify callback (will drive local TX output)
    callbacks_.on_key_event(key_down, last_key_timestamp_us_, callbacks_.context);

    // PTT management
    if (key_down) {
      // Activate PTT on first key-down
      if (!ptt_active_) {
        ptt_active_ = true;
        ESP_LOGD(kLogTag, "PTT activated");
      }
      // Extend PTT timeout
      ptt_timeout_us_ = last_key_timestamp_us_ + MillisecondsToMicroseconds(config_.ptt_tail_ms);
    }
  }
}

void RemoteCwServer::SendConnectAck() {
  ConnectPayload ack{};
  std::strncpy(ack.username, "SERVER", sizeof(ack.username) - 1);
  std::strncpy(ack.callsign, "SERVER", sizeof(ack.callsign) - 1);
  ack.permissions = 0;

  const size_t payload_size = sizeof(ack);
  const size_t frame_size = 2 + payload_size;

  if ((tx_tail_ + frame_size) % kTxBufferCapacity < tx_head_) {
    ESP_LOGW(kLogTag, "TX buffer full, cannot send CONNECT ACK");
    return;
  }

  tx_buffer_[tx_tail_] = kCmdConnect | kCmdMaskShort;
  tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;

  tx_buffer_[tx_tail_] = static_cast<uint8_t>(payload_size);
  tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;

  const uint8_t* ack_bytes = reinterpret_cast<const uint8_t*>(&ack);
  for (size_t i = 0; i < payload_size; ++i) {
    tx_buffer_[tx_tail_] = ack_bytes[i];
    tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
  }
}

void RemoteCwServer::SendPingResponse(uint8_t type, uint8_t sequence, uint32_t timestamp_ms) {
  uint8_t response[18];
  std::memset(response, 0, sizeof(response));
  response[0] = kCmdPing | kCmdMaskShort;
  response[1] = 16;  // Payload size
  response[2] = type;
  response[3] = sequence;
  std::memcpy(&response[4], &timestamp_ms, sizeof(uint32_t));

  const size_t free_space = (tx_tail_ >= tx_head_)
                                ? (kTxBufferCapacity - (tx_tail_ - tx_head_))
                                : (tx_head_ - tx_tail_);
  if (free_space >= sizeof(response)) {
    for (size_t i = 0; i < sizeof(response); ++i) {
      tx_buffer_[tx_tail_] = response[i];
      tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
    }
  }
}

void RemoteCwServer::TickRemotePtt(int64_t now_us) {
  if (ptt_active_ && now_us >= ptt_timeout_us_) {
    ptt_active_ = false;
    ESP_LOGD(kLogTag, "PTT deactivated (tail timeout)");
    // Note: Actual PTT GPIO control would be handled by callback context
  }
}

void RemoteCwServer::CloseClientSocket() {
  if (client_fd_ >= 0) {
    close(client_fd_);
    client_fd_ = -1;
  }
  client_ip_[0] = '\0';  // Clear client IP
  rx_bytes_ = 0;
  tx_head_ = 0;
  tx_tail_ = 0;
  last_key_timestamp_us_ = 0;
  ptt_active_ = false;
}

void RemoteCwServer::CloseListenSocket() {
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
}

uint32_t RemoteCwServer::DecodeTimestamp(uint8_t value) {
  value &= 0x7F;
  if (value <= 0x1F) {
    return value;
  }
  if (value <= 0x3F) {
    return static_cast<uint32_t>(32 + 4 * (value - 0x20));
  }
  return static_cast<uint32_t>(157 + 16 * (value - 0x40));
}

}  // namespace remote
