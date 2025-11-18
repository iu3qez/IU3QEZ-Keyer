#include "remote/remote_cw_client.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>

#include "audio/audio_stream_player.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include <fcntl.h>

namespace remote {
namespace {

constexpr char kLogTag[] = "RemoteCwClient";
constexpr uint32_t kPermissionsNone = 0x00;

template <typename T>
static void ZeroStruct(T* value) {
  if (value != nullptr) {
    std::memset(value, 0, sizeof(T));
  }
}

static uint32_t MicrosecondsToMilliseconds(int64_t value_us) {
  if (value_us <= 0) {
    return 0;
  }
  return static_cast<uint32_t>(value_us / 1000);
}

static int64_t MillisecondsToMicroseconds(uint32_t value_ms) {
  return static_cast<int64_t>(value_ms) * 1000;
}

}  // namespace

RemoteCwClient::RemoteCwClient() {
  ZeroStruct(&resolved_addr_);
  ZeroStruct(&rx_buffer_);
  ZeroStruct(&tx_buffer_);
}

RemoteCwClient::~RemoteCwClient() {
  Stop();

  // Delete task if it exists
  if (task_handle_ != nullptr) {
    vTaskDelete(task_handle_);
    task_handle_ = nullptr;
  }

  // Delete queues
  if (keying_queue_ != nullptr) {
    vQueueDelete(keying_queue_);
    keying_queue_ = nullptr;
  }
  if (cmd_queue_ != nullptr) {
    vQueueDelete(cmd_queue_);
    cmd_queue_ = nullptr;
  }
}

void RemoteCwClient::Configure(const RemoteCwClientConfig& config,
                               const RemoteCwClientCallbacks& callbacks) {
  ESP_EARLY_LOGW(kLogTag, "Configure() called - creating task and queues");
  config_ = config;
  callbacks_ = callbacks;

  // Create FreeRTOS queues
  ESP_EARLY_LOGW(kLogTag, "Creating keying queue (capacity: %d events)", kMaxKeyQueueDepth);
  keying_queue_ = xQueueCreate(kMaxKeyQueueDepth, sizeof(KeyingEvent));
  if (keying_queue_ == nullptr) {
    ESP_EARLY_LOGE(kLogTag, "Failed to create keying queue");
    return;
  }
  ESP_EARLY_LOGW(kLogTag, "Keying queue created successfully");

  ESP_EARLY_LOGW(kLogTag, "Creating command queue (capacity: 4 commands)");
  cmd_queue_ = xQueueCreate(4, sizeof(CommandMessage));  // Small queue for commands
  if (cmd_queue_ == nullptr) {
    ESP_EARLY_LOGE(kLogTag, "Failed to create command queue");
    return;
  }
  ESP_EARLY_LOGW(kLogTag, "Command queue created successfully");

  // Create FreeRTOS task
  ESP_EARLY_LOGW(kLogTag, "Creating FreeRTOS task (stack: 4KB, priority: %d)", tskIDLE_PRIORITY + 2);
  ESP_EARLY_LOGW(kLogTag, "sizeof(CommandMessage) = %u bytes", sizeof(CommandMessage));
  BaseType_t ret = xTaskCreate(
    TaskFunction,
    "remote_cw",
    4096,  // Stack size (4KB)
    this,  // Parameter (this pointer)
    tskIDLE_PRIORITY + 2,  // Priority
    &task_handle_
  );

  if (ret != pdPASS) {
    ESP_EARLY_LOGE(kLogTag, "Failed to create RemoteCwClient task (xTaskCreate returned %d)", ret);
    task_handle_ = nullptr;
    return;
  }

  ESP_EARLY_LOGW(kLogTag, "RemoteCwClient configured (task created, queues ready, task_handle=%p)", task_handle_);
}

esp_err_t RemoteCwClient::Start() {
  if (config_.server_host == nullptr || config_.server_host[0] == '\0') {
    ESP_LOGE(kLogTag, "Server host not configured");
    return ESP_ERR_INVALID_ARG;
  }
  if (config_.callsign == nullptr || config_.callsign[0] == '\0') {
    ESP_LOGE(kLogTag, "Callsign is required for CONNECT payload");
    return ESP_ERR_INVALID_ARG;
  }

  // Send kStart command to task
  UBaseType_t queue_items = uxQueueMessagesWaiting(cmd_queue_);
  ESP_EARLY_LOGW(kLogTag, "Start(): queue has %u items before send", queue_items);
  ESP_EARLY_LOGW(kLogTag, "Start(): sending kStart command to task");
  CommandMessage cmd{};
  cmd.cmd = TaskCommand::kStart;
  ESP_EARLY_LOGW(kLogTag, "Start(): cmd.cmd = %d (should be 0), sizeof(cmd) = %u", static_cast<int>(cmd.cmd), sizeof(cmd));
  if (xQueueSend(cmd_queue_, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_EARLY_LOGE(kLogTag, "Failed to queue Start command (queue full)");
    return ESP_ERR_TIMEOUT;
  }

  ESP_EARLY_LOGW(kLogTag, "Start command sent to task successfully");
  return ESP_OK;
}

void RemoteCwClient::Stop() {
  if (cmd_queue_ == nullptr) {
    return;  // Not configured yet
  }

  // Send kStop command to task (block until accepted)
  CommandMessage cmd{};
  cmd.cmd = TaskCommand::kStop;
  xQueueSend(cmd_queue_, &cmd, portMAX_DELAY);

  ESP_LOGI(kLogTag, "Stop command sent to task");
}

//=============================================================================
// Public API - Keying Event Queue
//=============================================================================

bool RemoteCwClient::QueueKeyingEvent(bool key_active, int64_t timestamp_us) {
  if (keying_queue_ == nullptr) {
    return false;  // Not configured
  }

  KeyingEvent evt{key_active, timestamp_us};

  // Try non-blocking send (ISR-safe)
  if (xQueueSend(keying_queue_, &evt, 0) != pdTRUE) {
    // Queue full - drop event and increment counter
    dropped_keying_events_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW(kLogTag, "Keying queue full, event dropped (total: %lu)",
             static_cast<unsigned long>(dropped_keying_events_.load()));
    return false;
  }

  return true;
}

//=============================================================================
// FreeRTOS Task
//=============================================================================

void RemoteCwClient::TaskFunction(void* param) {
  ESP_EARLY_LOGW(kLogTag, "TaskFunction() entry - param=%p", param);
  RemoteCwClient* self = static_cast<RemoteCwClient*>(param);
  ESP_EARLY_LOGW(kLogTag, "TaskFunction() cast successful - self=%p, calling RunTaskLoop()", self);
  self->RunTaskLoop();  // Never returns
}

void RemoteCwClient::RunTaskLoop() {
  ESP_EARLY_LOGW(kLogTag, "RemoteCwClient task started");

  while (true) {
    // Process commands from main loop (non-blocking)
    ProcessCommandQueue();

    // State machine progression
    RemoteCwClientState current_state = state_.load(std::memory_order_acquire);

    switch (current_state) {
      case RemoteCwClientState::kIdle:
        // Wait for kStart command (long timeout to yield CPU)
        WaitForCommandOrTimeout(1000);  // 1 second
        break;

      case RemoteCwClientState::kResolving:
        AttemptResolution();
        if (resolved_addr_valid_) {
          TransitionTo(RemoteCwClientState::kConnecting);
        } else {
          ScheduleReconnect(esp_timer_get_time());
        }
        break;

      case RemoteCwClientState::kConnecting:
        AttemptConnect(esp_timer_get_time());
        break;

      case RemoteCwClientState::kHandshake:
      case RemoteCwClientState::kConnected:
        HandleConnectedState();
        break;

      case RemoteCwClientState::kError:
        WaitForCommandOrTimeout(config_.reconnect_delay_ms);
        // Check if it's time to retry
        int64_t now_us = esp_timer_get_time();
        if (next_reconnect_time_us_ != 0 && now_us >= next_reconnect_time_us_) {
          TransitionTo(RemoteCwClientState::kResolving);
        }
        break;
    }
  }
}

void RemoteCwClient::ProcessCommandQueue() {
  CommandMessage cmd;

  // Non-blocking receive (check every loop iteration)
  if (xQueueReceive(cmd_queue_, &cmd, 0) == pdTRUE) {
    ESP_EARLY_LOGW(kLogTag, "ProcessCommandQueue: received command %d", static_cast<int>(cmd.cmd));
    switch (cmd.cmd) {
      case TaskCommand::kStart:
        ESP_EARLY_LOGW(kLogTag, "ProcessCommandQueue: kStart received, current state=%d", static_cast<int>(state_.load()));
        if (state_.load() == RemoteCwClientState::kIdle) {
          next_reconnect_time_us_ = esp_timer_get_time();
          ESP_EARLY_LOGW(kLogTag, "ProcessCommandQueue: calling TransitionTo(kResolving)");
          TransitionTo(RemoteCwClientState::kResolving);
          ESP_EARLY_LOGW(kLogTag, "ProcessCommandQueue: TransitionTo returned, new state=%d", static_cast<int>(state_.load()));
        } else {
          ESP_EARLY_LOGW(kLogTag, "ProcessCommandQueue: ignoring kStart (state not Idle)");
        }
        break;

      case TaskCommand::kStop:
        CloseSocket();
        ResetConnectionState();
        handshake_complete_ = false;
        TransitionTo(RemoteCwClientState::kIdle);
        ESP_LOGI(kLogTag, "Stopped by command");
        break;

      case TaskCommand::kUpdateConfig:
        if (state_.load() == RemoteCwClientState::kIdle) {
          config_ = cmd.config;
          ESP_LOGI(kLogTag, "Config updated");
        } else {
          ESP_LOGW(kLogTag, "Cannot update config while active - stop first");
        }
        break;
    }
  }
}

void RemoteCwClient::WaitForCommandOrTimeout(uint32_t timeout_ms) {
  CommandMessage cmd;
  // Block on command queue with timeout (yields CPU while waiting)
  // If a command is received, put it back for ProcessCommandQueue to handle
  if (xQueueReceive(cmd_queue_, &cmd, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
    xQueueSendToFront(cmd_queue_, &cmd, 0);
  }
  // If timeout occurred (no command), just return
}

//=============================================================================
// State Transitions
//=============================================================================

void RemoteCwClient::TransitionTo(RemoteCwClientState new_state) {
  RemoteCwClientState old_state = state_.exchange(new_state, std::memory_order_release);

  if (old_state == new_state) {
    return;  // No change
  }

  // Log state transitions
  const char* state_names[] = {"Idle", "Resolving", "Connecting", "Handshake", "Connected", "Error"};
  const char* old_name = (old_state <= RemoteCwClientState::kError) ? state_names[static_cast<int>(old_state)] : "Unknown";
  const char* new_name = (new_state <= RemoteCwClientState::kError) ? state_names[static_cast<int>(new_state)] : "Unknown";
  ESP_LOGI(kLogTag, "State: %s -> %s", old_name, new_name);

  state_enter_time_us_ = esp_timer_get_time();

  // Invoke callback if registered
  if (callbacks_.on_state_changed != nullptr) {
    callbacks_.on_state_changed(new_state, callbacks_.context);
  }
}

void RemoteCwClient::ScheduleReconnect(int64_t now_us) {
  CloseSocket();
  ResetConnectionState();
  handshake_complete_ = false;
  next_reconnect_time_us_ =
      now_us + MillisecondsToMicroseconds(config_.reconnect_delay_ms);
  TransitionTo(RemoteCwClientState::kError);
}

void RemoteCwClient::AttemptResolution() {
  struct addrinfo hints;
  ZeroStruct(&hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_INET;

  struct addrinfo* result = nullptr;
  const int err =
      getaddrinfo(config_.server_host, nullptr, &hints, &result);
  if (err != 0 || result == nullptr) {
    ESP_LOGE(kLogTag, "DNS resolution failed for %s: %d", config_.server_host, err);
    resolved_addr_valid_ = false;
    if (result != nullptr) {
      freeaddrinfo(result);
    }
    return;
  }

  ZeroStruct(&resolved_addr_);
  std::memcpy(&resolved_addr_, result->ai_addr, result->ai_addrlen);
  resolved_addr_len_ = static_cast<socklen_t>(result->ai_addrlen);
  reinterpret_cast<struct sockaddr_in*>(&resolved_addr_)->sin_port =
      lwip_htons(config_.server_port);
  resolved_addr_valid_ = true;
  freeaddrinfo(result);

  // Log successful resolution
  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(reinterpret_cast<struct sockaddr_in*>(&resolved_addr_)->sin_addr),
            ip_str, INET_ADDRSTRLEN);
  ESP_LOGI(kLogTag, "DNS resolved %s -> %s:%u", config_.server_host, ip_str, config_.server_port);
}

void RemoteCwClient::AttemptConnect(int64_t now_us) {
  if (!resolved_addr_valid_) {
    TransitionTo(RemoteCwClientState::kResolving);
    return;
  }

  if (socket_fd_ < 0) {
    socket_fd_ = lwip_socket(resolved_addr_.ss_family, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd_ < 0) {
      ESP_LOGE(kLogTag, "socket() failed: errno=%d", errno);
      ScheduleReconnect(now_us);
      return;
    }

    const int flags = lwip_fcntl(socket_fd_, F_GETFL, 0);
    lwip_fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
    connect_in_progress_ = false;
  }

  if (!connect_in_progress_) {
    const int res = lwip_connect(socket_fd_,
                                 reinterpret_cast<struct sockaddr*>(&resolved_addr_),
                                 resolved_addr_len_);
    if (res == 0) {
      EnterHandshake(now_us);
      return;
    }

    if (errno == EINPROGRESS || errno == EALREADY) {
      connect_in_progress_ = true;
      return;
    }

    ESP_LOGE(kLogTag, "connect() failed: errno=%d", errno);
    ScheduleReconnect(now_us);
    return;
  }

  int error = 0;
  socklen_t error_len = sizeof(error);
  if (lwip_getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
    ESP_LOGE(kLogTag, "getsockopt(SO_ERROR) failed: errno=%d", errno);
    ScheduleReconnect(now_us);
    return;
  }

  if (error == 0) {
    connect_in_progress_ = false;
    EnterHandshake(now_us);
    return;
  }

  if (error == EINPROGRESS) {
    return;
  }

  ESP_LOGE(kLogTag, "connect error: %d", error);
  ScheduleReconnect(now_us);
}

void RemoteCwClient::EnterHandshake(int64_t now_us) {
  TransitionTo(RemoteCwClientState::kHandshake);
  handshake_complete_ = false;
  connect_frame_sent_ = false;  // Reset flag to allow CONNECT frame send
  connect_in_progress_ = false;
  pending_ping_id_ = 0;
  pending_ping_sent_us_ = 0;
  measured_latency_ms_.store(0, std::memory_order_relaxed);  // Atomic write
  rx_bytes_ = 0;
  tx_head_ = 0;
  tx_tail_ = 0;
  last_local_key_timestamp_us_ = 0;
  last_remote_key_timestamp_us_ = 0;

  // EXPERIMENTAL: Delay CONNECT frame population by 100ms
  // Hypothesis: Server DL4YHF needs time after TCP connect() to prepare for handshake
  // Before refactoring, Tick() polling introduced natural 10-20ms delay
  // Now select() sends immediately, might be too fast for server
  ESP_LOGW(kLogTag, "EnterHandshake: delaying CONNECT frame by 100ms to test server timing");
  // Do NOT populate frame yet - will be done after delay
  // Note: No need to flush keying queue here - task will drain it in HandleConnectedState
}

//=============================================================================
// Event-Driven I/O (Task Context)
//=============================================================================

void RemoteCwClient::HandleConnectedState() {
  // IMPORTANT: CheckPeriodicTasks() MUST run BEFORE select()!
  // This populates CONNECT frame during handshake delay, so select() sees tx_head != tx_tail
  CheckPeriodicTasks();

  // Setup select() for event-driven I/O
  fd_set readfds, writefds;
  FD_ZERO(&readfds);
  FD_ZERO(&writefds);

  if (socket_fd_ >= 0) {
    FD_SET(socket_fd_, &readfds);
    if (tx_head_ != tx_tail_) {  // Has data to send
      FD_SET(socket_fd_, &writefds);
    }
  }

  // Timeout: 50ms (allows periodic tasks)
  struct timeval timeout{0, 50000};  // 50ms

  int ready = select(socket_fd_ + 1, &readfds, &writefds, NULL, &timeout);

  if (ready > 0 && socket_fd_ >= 0) {
    if (FD_ISSET(socket_fd_, &readfds)) {
      HandleSocketRead();
    }
    if (FD_ISSET(socket_fd_, &writefds)) {
      HandleSocketWrite();
    }
  } else if (ready < 0) {
    ESP_LOGE(kLogTag, "select() failed: errno=%d", errno);
    ScheduleReconnect(esp_timer_get_time());
    return;
  }

  // Drain keying queue
  DrainKeyingQueue();
}

void RemoteCwClient::HandleSocketRead() {
  int64_t now_us = esp_timer_get_time();
  const ssize_t available_space = static_cast<ssize_t>(kRxBufferCapacity - rx_bytes_);

  ESP_LOGV(kLogTag, "HandleSocketRead: rx_bytes_=%zu, available_space=%zd", rx_bytes_, available_space);

  if (available_space > 0) {
    const ssize_t read_bytes = lwip_recv(socket_fd_, rx_buffer_ + rx_bytes_, available_space, 0);

    ESP_LOGV(kLogTag, "HandleSocketRead: recv() returned %zd bytes (errno=%d)", read_bytes, errno);

    if (read_bytes > 0) {
      ESP_LOGV(kLogTag, "HandleSocketRead: received %zd bytes, total rx_bytes now %zu", read_bytes, rx_bytes_ + read_bytes);
      rx_bytes_ += static_cast<size_t>(read_bytes);
      ParseIncomingFrames(now_us);
    } else if (read_bytes == 0) {
      ESP_LOGW(kLogTag, "Server closed connection (recv returned 0)");
      ESP_LOGW(kLogTag, "State at disconnect: %s, handshake_complete=%d, rx_bytes=%zu, tx_pending=%zu",
               state_.load() == RemoteCwClientState::kHandshake ? "kHandshake" : "kConnected",
               handshake_complete_, rx_bytes_,
               (tx_tail_ >= tx_head_) ? (tx_tail_ - tx_head_) : (kTxBufferCapacity - tx_head_ + tx_tail_));
      ScheduleReconnect(now_us);
    } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
      ESP_LOGE(kLogTag, "recv() failed: errno=%d (%s)", errno, strerror(errno));
      ScheduleReconnect(now_us);
    }
  }
}

void RemoteCwClient::HandleSocketWrite() {
  DrainTxBuffer();
}

void RemoteCwClient::DrainKeyingQueue() {
  KeyingEvent evt;

  // Drain all events in queue (non-blocking)
  while (xQueueReceive(keying_queue_, &evt, 0) == pdTRUE) {
    // Calculate delta from last event
    uint32_t delta_ms = 0;
    if (last_local_key_timestamp_us_ > 0 && evt.timestamp_us > last_local_key_timestamp_us_) {
      delta_ms = MicrosecondsToMilliseconds(evt.timestamp_us - last_local_key_timestamp_us_);
    }

    if (delta_ms > kMaxTimestampMs) {
      delta_ms = kMaxTimestampMs;
    }

    // Encode MORSE frame
    const uint8_t cw_byte = EncodeTimestamp(delta_ms) | (evt.key_active ? 0x80 : 0x00);
    const uint8_t command = static_cast<uint8_t>(kCmdMorse | kCmdMaskShort);
    const uint8_t length = 1;

    // Queue to TX buffer
    const size_t required = 3;
    const size_t free_space = (tx_tail_ >= tx_head_) ? (kTxBufferCapacity - (tx_tail_ - tx_head_) - 1)
                                                     : (tx_head_ - tx_tail_ - 1);

    if (free_space >= required) {
      tx_buffer_[tx_tail_] = command;
      tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
      tx_buffer_[tx_tail_] = length;
      tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
      tx_buffer_[tx_tail_] = cw_byte;
      tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;

      last_local_key_timestamp_us_ = evt.timestamp_us;
      last_keying_activity_us_ = esp_timer_get_time();
    } else {
      ESP_LOGW(kLogTag, "TX buffer full, keying event lost");
      break;
    }
  }

  // PTT management
  if (last_keying_activity_us_ > 0 && !ptt_active_) {
    SendPttCommand(true);  // PTT ON
  }
}

void RemoteCwClient::CheckPeriodicTasks() {
  int64_t now_us = esp_timer_get_time();

  // EXPERIMENTAL: Delayed CONNECT frame send (100ms after entering Handshake state)
  if (state_.load() == RemoteCwClientState::kHandshake && !handshake_complete_) {
    // Check if CONNECT frame not yet sent (use flag, NOT buffer check!)
    // IMPORTANT: tx_head == tx_tail is TRUE both before AND after send, causing duplicates!
    if (!connect_frame_sent_) {
      // Wait 100ms after entering Handshake state before sending CONNECT
      constexpr uint32_t kConnectDelayMs = 100;
      if (now_us - state_enter_time_us_ >= MillisecondsToMicroseconds(kConnectDelayMs)) {
        ESP_LOGW(kLogTag, "100ms delay elapsed, now populating CONNECT frame (ONCE)");
        PopulateConnectFrame();
        connect_frame_sent_ = true;  // Mark as sent to prevent duplicates
        ESP_LOGI(kLogTag, "CONNECT frame queued (callsign: %s), will be sent by select() when socket ready", config_.callsign);
        // select() in HandleConnectedState() will detect tx_head != tx_tail and drain buffer
      }
    }
  }

  // Handshake timeout
  if (state_.load() == RemoteCwClientState::kHandshake && !handshake_complete_) {
    if (now_us - state_enter_time_us_ >= MillisecondsToMicroseconds(config_.handshake_timeout_ms)) {
      ESP_LOGE(kLogTag, "Handshake timeout");
      ScheduleReconnect(now_us);
      return;
    }
  }

  // Ping interval
  if (handshake_complete_ &&
      now_us - last_ping_time_us_ >= MillisecondsToMicroseconds(config_.ping_interval_ms)) {
    SendPingRequest(now_us);
  }

  // PTT timeout
  if (ptt_active_ && last_keying_activity_us_ > 0) {
    uint32_t latency = measured_latency_ms_.load(std::memory_order_relaxed);
    if (now_us - last_keying_activity_us_ >= MillisecondsToMicroseconds(config_.ptt_tail_ms + latency)) {
      SendPttCommand(false);  // PTT OFF
      last_keying_activity_us_ = 0;  // Reset to prevent PTT ON/OFF loop
      DrainTxBuffer();
    }
  }
}

void RemoteCwClient::DrainTxBuffer() {
  if (socket_fd_ < 0) {
    return;
  }

  ESP_LOGV(kLogTag, "DrainTxBuffer: tx_head=%zu, tx_tail=%zu, pending=%zu bytes",
           tx_head_, tx_tail_,
           (tx_tail_ >= tx_head_) ? (tx_tail_ - tx_head_) : (kTxBufferCapacity - tx_head_ + tx_tail_));

  while (tx_head_ != tx_tail_) {
    const size_t contiguous =
        (tx_tail_ > tx_head_) ? (tx_tail_ - tx_head_) : (kTxBufferCapacity - tx_head_);

    ESP_LOGV(kLogTag, "DrainTxBuffer: trying to send %zu bytes (cmd=0x%02X)", contiguous, tx_buffer_[tx_head_]);

    const ssize_t sent =
        lwip_send(socket_fd_, tx_buffer_ + tx_head_, contiguous, 0);

    if (sent > 0) {
      ESP_LOGV(kLogTag, "DrainTxBuffer: sent %zd of %zu bytes (cmd=0x%02X)", sent, contiguous, tx_buffer_[tx_head_]);

      // Log details for MORSE frames (0x50 = CMD_MORSE | SHORT_BLOCK)
      if (contiguous >= 3 && tx_buffer_[tx_head_] == 0x50) {
        ESP_LOGI(kLogTag, "Sent MORSE: %d bytes starting at buffer[%zu]: {0x%02X, 0x%02X, 0x%02X, ...}",
                 static_cast<int>(sent), tx_head_,
                 tx_buffer_[tx_head_], tx_buffer_[tx_head_ + 1], tx_buffer_[tx_head_ + 2]);
      }
      // Log CONNECT frames (0x41 = CMD_CONNECT | SHORT_BLOCK)
      else if (tx_buffer_[tx_head_] == 0x41) {
        ESP_LOGV(kLogTag, "Sent CONNECT frame: %zd bytes", sent);
      }

      tx_head_ = (tx_head_ + static_cast<size_t>(sent)) % kTxBufferCapacity;
    } else {
      if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS) {
        ESP_LOGV(kLogTag, "send() would block, deferring %zu bytes (errno=%d)", contiguous, errno);
        break;
      }
      ESP_LOGE(kLogTag, "send() failed: errno=%d (%s) while sending %zu bytes",
               errno, strerror(errno), contiguous);
      ScheduleReconnect(esp_timer_get_time());
      break;
    }
  }

  ESP_LOGV(kLogTag, "DrainTxBuffer: done, tx_head=%zu, tx_tail=%zu, remaining=%zu bytes",
           tx_head_, tx_tail_,
           (tx_tail_ >= tx_head_) ? (tx_tail_ - tx_head_) : (kTxBufferCapacity - tx_head_ + tx_tail_));
}

void RemoteCwClient::PopulateConnectFrame() {
  ConnectPayload payload{};
  std::memset(&payload, 0, sizeof(payload));

  // CWNet server expects callsign in lowercase
  char lowercase_callsign[sizeof(payload.callsign)];
  std::memset(lowercase_callsign, 0, sizeof(lowercase_callsign));
  const char* src = config_.callsign;
  for (size_t i = 0; i < sizeof(lowercase_callsign) - 1 && src[i] != '\0'; ++i) {
    lowercase_callsign[i] = static_cast<char>(tolower(static_cast<unsigned char>(src[i])));
  }

  std::strncpy(payload.username, lowercase_callsign, sizeof(payload.username) - 1);
  std::strncpy(payload.callsign, lowercase_callsign, sizeof(payload.callsign) - 1);
  // CWNet protocol uses little-endian (LSB first) for all multi-byte fields.
  // ESP32 is little-endian native, so no conversion needed.
  payload.permissions = kPermissionsNone;

  ESP_LOGI(kLogTag, "CONNECT payload: username='%s', callsign='%s', permissions=0x%08lx, size=%zu",
           payload.username, payload.callsign, static_cast<unsigned long>(payload.permissions),
           sizeof(payload));

  const uint8_t command = static_cast<uint8_t>(kCmdConnect | kCmdMaskShort);
  const uint8_t length = static_cast<uint8_t>(sizeof(payload));

  const size_t required =
      2 + sizeof(payload);  // command + length + payload.
  const size_t free_space =
      (tx_tail_ >= tx_head_) ? (kTxBufferCapacity - (tx_tail_ - tx_head_))
                             : (tx_head_ - tx_tail_);
  if (free_space < required) {
    ESP_LOGE(kLogTag, "TX buffer exhausted while queuing CONNECT frame");
    return;
  }

  tx_buffer_[tx_tail_] = command;
  tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
  tx_buffer_[tx_tail_] = length;
  tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;

  const uint8_t* payload_bytes = reinterpret_cast<const uint8_t*>(&payload);
  for (size_t i = 0; i < sizeof(payload); ++i) {
    tx_buffer_[tx_tail_] = payload_bytes[i];
    tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
  }
}

void RemoteCwClient::SendPingRequest(int64_t now_us) {
  uint8_t frame[18];
  std::memset(frame, 0, sizeof(frame));
  const uint8_t command = static_cast<uint8_t>(kCmdPing | kCmdMaskShort);
  const uint8_t length = 16;
  frame[0] = command;
  frame[1] = length;
  frame[2] = 0;  // payload[0]: type = request
  frame[3] = pending_ping_id_;  // payload[1]: sequence ID
  frame[4] = 0;  // payload[2]: reserved
  frame[5] = 0;  // payload[3]: reserved
  const uint32_t t0_ms = MicrosecondsToMilliseconds(now_us);
  // Protocol uses little-endian. ESP32 is LE native, so no conversion needed.
  std::memcpy(&frame[6], &t0_ms, sizeof(uint32_t));  // payload[4-7]: t0

  ESP_LOGD(kLogTag, "PING REQUEST: id=%u, t0=%lu ms", pending_ping_id_,
           static_cast<unsigned long>(t0_ms));

  const size_t required = sizeof(frame);
  const size_t free_space =
      (tx_tail_ >= tx_head_) ? (kTxBufferCapacity - (tx_tail_ - tx_head_))
                             : (tx_head_ - tx_tail_);
  if (free_space < required) {
    ESP_LOGW(kLogTag, "Unable to queue ping request, TX buffer full");
    return;
  }

  for (size_t i = 0; i < sizeof(frame); ++i) {
    tx_buffer_[tx_tail_] = frame[i];
    tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
  }

  pending_ping_sent_us_ = now_us;
  last_ping_time_us_ = now_us;
}

void RemoteCwClient::SendPttCommand(bool ptt_on) {
  // Send "set_ptt 1\n" or "set_ptt 0\n" as RIGCTLD command (0x06)
  const char* cmd = ptt_on ? "set_ptt 1\n" : "set_ptt 0\n";
  const size_t cmd_len = std::strlen(cmd) + 1;  // Include null terminator (11 bytes)

  const size_t required = 2 + cmd_len;  // command + length + string
  const size_t free_space =
      (tx_tail_ >= tx_head_) ? (kTxBufferCapacity - (tx_tail_ - tx_head_) - 1)
                             : (tx_head_ - tx_tail_ - 1);

  if (free_space < required) {
    ESP_LOGW(kLogTag, "TX buffer full, cannot send PTT command");
    return;
  }

  // Build PTT command (0x46 = 0x06 | SHORT_BLOCK)
  tx_buffer_[tx_tail_] = static_cast<uint8_t>(kCmdRigCtld | kCmdMaskShort);
  tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
  tx_buffer_[tx_tail_] = static_cast<uint8_t>(cmd_len);
  tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;

  for (size_t i = 0; i < cmd_len; ++i) {
    tx_buffer_[tx_tail_] = static_cast<uint8_t>(cmd[i]);
    tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
  }

  ptt_active_ = ptt_on;
  ESP_LOGI(kLogTag, "PTT %s queued", ptt_on ? "ON" : "OFF");
}

// Old FlushKeyingQueue removed - replaced by DrainKeyingQueue() in task

void RemoteCwClient::ParseIncomingFrames(int64_t now_us) {
  while (true) {
    size_t frame_size = 0;
    uint8_t command = 0;
    size_t payload_offset = 0;
    size_t payload_size = 0;
    if (!TryExtractFrame(&frame_size, &command, &payload_offset, &payload_size)) {
      break;
    }

    const uint8_t* payload = rx_buffer_ + payload_offset;
    HandleFrame(command & kCmdMaskCommand, payload, payload_size, now_us);

    const size_t remaining = rx_bytes_ - frame_size;
    if (remaining > 0) {
      std::memmove(rx_buffer_, rx_buffer_ + frame_size, remaining);
    }
    rx_bytes_ = remaining;
  }
}

bool RemoteCwClient::TryExtractFrame(size_t* frame_size, uint8_t* command,
                                     size_t* payload_offset, size_t* payload_size) const {
  if (rx_bytes_ == 0) {
    return false;
  }

  const uint8_t raw_command = rx_buffer_[0];
  FrameCategory category = FrameCategory::kNoPayload;
  if ((raw_command & kCmdMaskBlockLen) == 0) {
    category = FrameCategory::kNoPayload;
  } else if ((raw_command & kCmdMaskBlockLen) == kCmdMaskShort) {
    category = FrameCategory::kShortPayload;
  } else if ((raw_command & kCmdMaskBlockLen) == kCmdMaskLong) {
    category = FrameCategory::kLongPayload;
  } else {
    category = FrameCategory::kReserved;
  }

  size_t required = 1;
  size_t payload_len = 0;
  size_t offset = 1;

  switch (category) {
    case FrameCategory::kNoPayload:
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
  *command = raw_command;
  *payload_offset = offset;
  *payload_size = payload_len;
  return true;
}

void RemoteCwClient::HandleFrame(uint8_t command, const uint8_t* payload,
                                 size_t payload_size, int64_t now_us) {
  switch (command) {
    case kCmdConnect:
      HandleConnectAck(payload, payload_size);
      break;
    case kCmdPing:
      HandlePingFrame(payload, payload_size, now_us);
      break;
    case kCmdMorse:
      HandleMorseFrame(payload, payload_size, now_us);
      break;
    case kCmdPrint:
      HandlePrintFrame(payload, payload_size);
      break;
    case kCmdTxInfo:
      HandleStubFrame(command, "TxInfo");
      break;
    case kCmdRigCtld:
      HandleStubFrame(command, "RigCtld");
      break;
    case kCmdAudio:
      HandleAudioFrame(payload, payload_size);
      break;
    case kCmdVorbis:
      HandleStubFrame(command, "Vorbis");
      break;
    case kCmdCiV:
      HandleStubFrame(command, "CiV");
      break;
    case kCmdSpectrum:
      HandleStubFrame(command, "Spectrum");
      break;
    case kCmdFreqReport:
      HandleStubFrame(command, "FreqReport");
      break;
    case kCmdMeterReport:
      HandleStubFrame(command, "MeterReport");
      break;
    case kCmdPotiReport:
      HandleStubFrame(command, "PotiReport");
      break;
    case kCmdTunnel1:
    case kCmdTunnel2:
    case kCmdTunnel3:
      HandleStubFrame(command, "Tunnel");
      break;
    default:
      ESP_LOGW(kLogTag, "Received unknown command 0x%02X", command);
      break;
  }
}

void RemoteCwClient::HandleConnectAck(const uint8_t* payload, size_t payload_size) {
  if (payload_size < sizeof(ConnectPayload)) {
    ESP_LOGW(kLogTag, "CONNECT ack payload too small (%u), expected %zu",
             static_cast<unsigned>(payload_size), sizeof(ConnectPayload));
    return;
  }

  const ConnectPayload* ack_payload = reinterpret_cast<const ConnectPayload*>(payload);
  ESP_LOGI(kLogTag, "CONNECT ACK received: username='%s', callsign='%s', permissions=0x%08lx",
           ack_payload->username, ack_payload->callsign,
           static_cast<unsigned long>(ack_payload->permissions));

  handshake_complete_ = true;
  TransitionTo(RemoteCwClientState::kConnected);
  ESP_LOGI(kLogTag, "Remote CW handshake complete");
}

void RemoteCwClient::HandlePingFrame(const uint8_t* payload, size_t payload_size,
                                     int64_t now_us) {
  if (payload_size != 16) {
    ESP_LOGW(kLogTag, "Invalid ping payload size: %u", static_cast<unsigned>(payload_size));
    return;
  }

  const uint8_t type = payload[0];
  const uint8_t sequence = payload[1];
  int64_t frame_time_us = now_us;

  switch (type) {
    case 0: {
      // Server initiated ping. Respond with type 1 containing server's t0 and our t1.
      // First, synchronize our timer with the server's timer (like the official client does)
      uint32_t server_t0_ms;
      std::memcpy(&server_t0_ms, &payload[4], 4);
      const uint32_t our_time_ms = MicrosecondsToMilliseconds(frame_time_us);
      // Calculate offset to sync our timer with server's timer
      timer_sync_offset_ms_ = static_cast<int32_t>(server_t0_ms) - static_cast<int32_t>(our_time_ms);

      uint8_t response[18];
      std::memset(response, 0, sizeof(response));
      response[0] = static_cast<uint8_t>(kCmdPing | kCmdMaskShort);
      response[1] = 16;
      response[2] = 1;  // payload[0]: type = response_1
      response[3] = sequence;  // payload[1]: sequence ID
      response[4] = 0;  // payload[2]: reserved
      response[5] = 0;  // payload[3]: reserved
      // Copy t0 from server's request (payload[4-7] → response payload[4-7])
      std::memcpy(&response[6], &payload[4], 4);
      // Add our t1 timestamp (response payload[8-11]), synchronized with server's timer
      const uint32_t synced_t1_ms = our_time_ms + timer_sync_offset_ms_;
      // Protocol uses little-endian. ESP32 is LE native, so no conversion needed.
      std::memcpy(&response[10], &synced_t1_ms, sizeof(uint32_t));
      const size_t free_space =
          (tx_tail_ >= tx_head_) ? (kTxBufferCapacity - (tx_tail_ - tx_head_))
                                 : (tx_head_ - tx_tail_);
      if (free_space >= sizeof(response)) {
        for (size_t i = 0; i < sizeof(response); ++i) {
          tx_buffer_[tx_tail_] = response[i];
          tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
        }
      }
      break;
    }
    case 1: {
      // Server responded to our request. Extract RTT and send final ack.
      // First, synchronize our timer with server's timer using t1 from the response
      uint32_t server_t1_ms;
      std::memcpy(&server_t1_ms, &payload[8], 4);  // Extract t1 from payload[8-11]
      const uint32_t our_time_ms = MicrosecondsToMilliseconds(frame_time_us);
      timer_sync_offset_ms_ = static_cast<int32_t>(server_t1_ms) - static_cast<int32_t>(our_time_ms);

      if (sequence == pending_ping_id_ && pending_ping_sent_us_ != 0) {
        const int64_t rtt_us = now_us - pending_ping_sent_us_;
        uint32_t latency_ms = MicrosecondsToMilliseconds(rtt_us / 2);
        measured_latency_ms_.store(latency_ms, std::memory_order_relaxed);  // Atomic write
        if (callbacks_.on_latency_updated != nullptr) {
          callbacks_.on_latency_updated(latency_ms, callbacks_.context);
        }
      }

      uint8_t ack[18];
      std::memset(ack, 0, sizeof(ack));
      ack[0] = static_cast<uint8_t>(kCmdPing | kCmdMaskShort);
      ack[1] = 16;
      ack[2] = 2;  // payload[0]: type = response_2
      ack[3] = sequence;  // payload[1]: sequence ID
      ack[4] = 0;  // payload[2]: reserved
      ack[5] = 0;  // payload[3]: reserved
      // Copy t0 and t1 from server's response (payload[4-11] → ack payload[4-11])
      std::memcpy(&ack[6], &payload[4], 8);
      // Add our t2 timestamp (ack payload[12-15]), synchronized with server's timer
      const uint32_t synced_t2_ms = our_time_ms + timer_sync_offset_ms_;
      // Protocol uses little-endian. ESP32 is LE native, so no conversion needed.
      std::memcpy(&ack[14], &synced_t2_ms, sizeof(uint32_t));

      ESP_LOGD(kLogTag, "PING RTT: id=%u", sequence);
      const size_t free_space =
          (tx_tail_ >= tx_head_) ? (kTxBufferCapacity - (tx_tail_ - tx_head_))
                                 : (tx_head_ - tx_tail_);
      if (free_space >= sizeof(ack)) {
        for (size_t i = 0; i < sizeof(ack); ++i) {
          tx_buffer_[tx_tail_] = ack[i];
          tx_tail_ = (tx_tail_ + 1) % kTxBufferCapacity;
        }
      }
      pending_ping_id_++;
      pending_ping_sent_us_ = 0;
      last_ping_time_us_ = now_us;
      break;
    }
    case 2:
      pending_ping_id_++;
      break;
    default:
      ESP_LOGW(kLogTag, "Unknown ping type %u", static_cast<unsigned>(type));
      break;
  }
}

void RemoteCwClient::HandleMorseFrame(const uint8_t* payload, size_t payload_size,
                                      int64_t now_us) {
  if (callbacks_.on_remote_key_event == nullptr) {
    return;
  }

  for (size_t i = 0; i < payload_size; ++i) {
    const uint8_t cw_byte = payload[i];
    const bool key_down = (cw_byte & 0x80) != 0;
    const uint8_t ts_encoded = cw_byte & 0x7F;
    const uint32_t delta_ms = DecodeTimestamp(ts_encoded);
    const int64_t delta_us = MillisecondsToMicroseconds(delta_ms);

    if (last_remote_key_timestamp_us_ == 0) {
      last_remote_key_timestamp_us_ = now_us;
    } else {
      last_remote_key_timestamp_us_ += delta_us;
    }

    callbacks_.on_remote_key_event(key_down, last_remote_key_timestamp_us_,
                                   callbacks_.context);
  }
}

void RemoteCwClient::HandlePrintFrame(const uint8_t* payload, size_t payload_size) {
  if (callbacks_.on_print_message != nullptr && payload_size > 0) {
    // Invoke callback with the raw message payload.
    // The message is NOT null-terminated by the protocol, so we pass length.
    callbacks_.on_print_message(reinterpret_cast<const char*>(payload), payload_size,
                               callbacks_.context);
  }
}

void RemoteCwClient::HandleAudioFrame(const uint8_t* payload, size_t payload_size) {
  // Check if audio streaming is enabled via configuration
  if (!config_.stream_audio) {
    return;  // Silently discard audio frames when streaming is disabled
  }

  if (audio_stream_player_ != nullptr && payload_size > 0) {
    // Update volume if needed (volume is checked internally during ReadStereoFrames)
    if (audio_stream_player_->GetVolume() != config_.stream_volume) {
      audio_stream_player_->SetVolume(config_.stream_volume);
    }

    // Write A-Law samples directly to AudioStreamPlayer
    const size_t written = audio_stream_player_->WriteALawSamples(payload, payload_size);

    if (written < payload_size) {
      ESP_LOGW(kLogTag, "AudioStreamPlayer buffer full: dropped %zu/%zu samples",
               payload_size - written, payload_size);
    }
  }
}

void RemoteCwClient::HandleStubFrame(uint8_t command, const char* command_name) {
  ESP_LOGD(kLogTag, "Received unimplemented command 0x%02X (%s) - ignoring (STUB)", command,
           command_name);
}

void RemoteCwClient::ResetConnectionState() {
  resolved_addr_valid_ = false;
  rx_bytes_ = 0;
  tx_head_ = 0;
  tx_tail_ = 0;
  connect_in_progress_ = false;
}

void RemoteCwClient::CloseSocket() {
  if (socket_fd_ >= 0) {
    lwip_close(socket_fd_);
    socket_fd_ = -1;
  }
}

uint8_t RemoteCwClient::EncodeTimestamp(uint32_t milliseconds) {
  if (milliseconds <= 31) {
    return static_cast<uint8_t>(milliseconds & 0x7F);
  }
  if (milliseconds <= 156) {
    return static_cast<uint8_t>(0x20 + ((milliseconds - 32) / 4));
  }
  if (milliseconds <= 1165) {
    return static_cast<uint8_t>(0x40 + ((milliseconds - 157) / 16));
  }
  return 0x7F;
}

uint32_t RemoteCwClient::DecodeTimestamp(uint8_t value) {
  value &= 0x7F;
  if (value <= 0x1F) {
    return value;
  }
  if (value <= 0x3F) {
    return static_cast<uint32_t>(32 + 4 * (value - 0x20));
  }
  return static_cast<uint32_t>(157 + 16 * (value - 0x40));
}

void RemoteCwClient::DumpDiagnostics() const {
  ESP_EARLY_LOGW(kLogTag, "=== RemoteCwClient Diagnostics ===");
  ESP_EARLY_LOGW(kLogTag, "Task handle: %p", task_handle_);
  ESP_EARLY_LOGW(kLogTag, "Keying queue: %p", keying_queue_);
  ESP_EARLY_LOGW(kLogTag, "Command queue: %p", cmd_queue_);
  ESP_EARLY_LOGW(kLogTag, "State: %d", static_cast<int>(state_.load()));
  ESP_EARLY_LOGW(kLogTag, "Dropped events: %lu", static_cast<unsigned long>(dropped_keying_events_.load()));
  ESP_EARLY_LOGW(kLogTag, "Latency: %lu ms", static_cast<unsigned long>(measured_latency_ms_.load()));

  if (task_handle_ != nullptr) {
    ESP_EARLY_LOGW(kLogTag, "Task exists - should be running");
  } else {
    ESP_EARLY_LOGE(kLogTag, "Task handle is NULL - task was never created!");
  }

  if (keying_queue_ == nullptr) {
    ESP_EARLY_LOGE(kLogTag, "Keying queue is NULL - Configure() was never called or failed!");
  }

  if (cmd_queue_ == nullptr) {
    ESP_EARLY_LOGE(kLogTag, "Command queue is NULL - Configure() was never called or failed!");
  }
}

}  // namespace remote
