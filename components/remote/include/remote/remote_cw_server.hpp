#pragma once

/**
 * @file remote_cw_server.hpp
 * @brief CWNet server for receiving remote keying (partial implementation)
 *
 * This is a minimal CWNet server implementation for testing and peer-to-peer operation.
 * Supports single client connection only.
 *
 * Implemented features:
 * - CONNECT handshake
 * - PING latency measurement
 * - MORSE frame reception → PTT + local keying output
 *
 * NOT implemented (stubs):
 * - Multiple clients
 * - PRINT, TxInfo, RigCtld, Audio, Vorbis, CiV, Spectrum, Tunnel commands
 * - Client authentication/permissions
 */

#include <cstdint>
#include "esp_err.h"
#include "lwip/sockets.h"

namespace remote {

/**
 * @brief Operational state of the RemoteCwServer state machine.
 */
enum class RemoteCwServerState : uint8_t {
  kIdle = 0,        // Not listening
  kListening,       // Waiting for client connection
  kHandshake,       // Client connected, CONNECT handshake in progress
  kConnected,       // Client connected and authenticated
  kError,           // Error occurred, will restart if enabled
};

/**
 * @brief Configuration for RemoteCwServer.
 */
struct RemoteCwServerConfig {
  uint16_t listen_port = 7355;  // TCP port to listen on (default CWNet port)
  bool auto_restart = true;     // Automatically restart on error/disconnect
  uint32_t ptt_tail_ms = 200;   // PTT tail delay after last key-up (ms)
};

/**
 * @brief Callback hooks for server events.
 */
struct RemoteCwServerCallbacks {
  void (*on_state_changed)(RemoteCwServerState new_state, void* context) = nullptr;
  void (*on_key_event)(bool key_down, int64_t timestamp_us, void* context) = nullptr;
  void* context = nullptr;
};

/**
 * @brief Minimal CWNet server for testing and P2P operation.
 *
 * Single-client server that receives MORSE frames and controls PTT/keying output.
 * Non-blocking polling state machine (no FreeRTOS tasks).
 */
class RemoteCwServer {
 public:
  RemoteCwServer();
  ~RemoteCwServer();

  RemoteCwServer(const RemoteCwServer&) = delete;
  RemoteCwServer& operator=(const RemoteCwServer&) = delete;

  /**
   * @brief Configure server with port and callbacks.
   */
  void Configure(const RemoteCwServerConfig& config, const RemoteCwServerCallbacks& callbacks);

  /**
   * @brief Start listening for connections.
   */
  esp_err_t Start();

  /**
   * @brief Stop server and close connections.
   */
  void Stop();

  /**
   * @brief Progress the server state machine.
   * @param now_us Current timestamp in microseconds.
   */
  void Tick(int64_t now_us);

  /**
   * @brief Get current server state.
   */
  RemoteCwServerState state() const { return state_; }

  /**
   * @brief Get connected client IP address.
   * @return Client IP string or nullptr if no client connected.
   */
  const char* client_ip() const { return (client_fd_ >= 0) ? client_ip_ : nullptr; }

 private:
  enum class FrameCategory : uint8_t {
    kNoPayload = 0,
    kShortPayload,
    kLongPayload,
    kReserved,
  };

  struct __attribute__((packed)) ConnectPayload {
    char username[44];
    char callsign[44];
    uint32_t permissions;
  };

  static constexpr uint8_t kCmdMaskBlockLen = 0xC0;
  static constexpr uint8_t kCmdMaskCommand = 0x3F;
  static constexpr uint8_t kCmdMaskShort = 0x40;
  static constexpr uint8_t kCmdMaskLong = 0x80;
  static constexpr uint8_t kCmdConnect = 0x01;
  static constexpr uint8_t kCmdPing = 0x03;
  static constexpr uint8_t kCmdMorse = 0x10;

  static constexpr size_t kRxBufferCapacity = 1024;
  static constexpr size_t kTxBufferCapacity = 1024;

  void ChangeState(RemoteCwServerState new_state);
  void HandleNewConnection();
  void HandleSocketIo(int64_t now_us);
  void DrainTxBuffer();
  void ParseIncomingFrames(int64_t now_us);
  bool TryExtractFrame(size_t* frame_size, uint8_t* command, size_t* payload_offset,
                       size_t* payload_size) const;
  void HandleFrame(uint8_t command, const uint8_t* payload, size_t payload_size, int64_t now_us);
  void HandleConnectFrame(const uint8_t* payload, size_t payload_size);
  void HandlePingFrame(const uint8_t* payload, size_t payload_size);
  void HandleMorseFrame(const uint8_t* payload, size_t payload_size, int64_t now_us);
  void SendConnectAck();
  void SendPingResponse(uint8_t type, uint8_t sequence, uint32_t timestamp_ms);
  void TickRemotePtt(int64_t now_us);
  void CloseClientSocket();
  void CloseListenSocket();

  static uint32_t DecodeTimestamp(uint8_t value);

  RemoteCwServerConfig config_{};
  RemoteCwServerCallbacks callbacks_{};
  RemoteCwServerState state_ = RemoteCwServerState::kIdle;

  int listen_fd_ = -1;        // Listen socket
  int client_fd_ = -1;        // Connected client socket
  char client_ip_[16] = "";   // Connected client IP address
  int64_t state_enter_time_us_ = 0;

  uint8_t rx_buffer_[kRxBufferCapacity];
  size_t rx_bytes_ = 0;

  uint8_t tx_buffer_[kTxBufferCapacity];
  size_t tx_head_ = 0;
  size_t tx_tail_ = 0;

  // PTT management (for received keying)
  bool ptt_active_ = false;
  int64_t ptt_timeout_us_ = 0;
  int64_t last_key_timestamp_us_ = 0;
};

}  // namespace remote
