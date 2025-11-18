#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace audio {
class AudioStreamPlayer;
}

namespace remote {

/**
 * @brief Operational state of the RemoteCwClient state machine.
 */
enum class RemoteCwClientState : uint8_t {
  kIdle = 0,
  kResolving,
  kConnecting,
  kHandshake,
  kConnected,
  kError,
};

/**
 * @brief Configuration required to start the RemoteCwClient.
 */
struct RemoteCwClientConfig {
  const char* server_host = nullptr;
  uint16_t server_port = 7355;  // Default CWNet port per protocol documentation.
  const char* callsign = nullptr;  // Username and callsign share the same value (station ID).
  uint32_t reconnect_delay_ms = 5000;  // Wait 5s before retrying to avoid aggressive reconnect loops.
  uint32_t ping_interval_ms = 2000;    // Send latency probes every 2s to track round-trip time.
  uint32_t handshake_timeout_ms = 3000;  // Abort handshake if CONNECT/ACK not completed within 3s.
  uint32_t ptt_tail_ms = 200;  // PTT tail delay after last keying event.
  bool stream_audio = false;   // Enable remote audio streaming (RX mode).
  uint8_t stream_volume = 100; // Remote audio stream volume (0-100%).
};

/**
 * @brief Callback hooks for higher-level components.
 */
struct RemoteCwClientCallbacks {
  void (*on_state_changed)(RemoteCwClientState new_state, void* context) = nullptr;
  void (*on_remote_key_event)(bool key_down, int64_t timestamp_us, void* context) = nullptr;
  void (*on_latency_updated)(uint32_t latency_ms, void* context) = nullptr;
  void (*on_print_message)(const char* message, size_t length, void* context) = nullptr;
  void* context = nullptr;
};

/**
 * @brief Keying event for FreeRTOS queue (ISR-safe communication).
 */
struct KeyingEvent {
  bool key_active;       // true = CW key closed (transmitting), false = key open
  int64_t timestamp_us;  // Microsecond timestamp of keying state change
};

/**
 * @brief Task command types for main loop → task communication.
 */
enum class TaskCommand : uint8_t {
  kStart,         // Begin connection attempts (transition from Idle to Resolving)
  kStop,          // Close socket and return to Idle state
  kUpdateConfig,  // Reconfigure server/port/callsign (only valid when Idle)
};

/**
 * @brief Command message for FreeRTOS queue.
 */
struct CommandMessage {
  TaskCommand cmd;
  RemoteCwClientConfig config;  // Only used for kUpdateConfig
};

/**
 * @brief CWNet client responsible for remote keying transport.
 *
 * The client uses a dedicated FreeRTOS task with event-driven socket I/O
 * (select() blocking) to minimize CPU overhead. Main loop communicates with
 * the task via FreeRTOS queues for keying events and commands.
 *
 * Architecture (2025-11-15 refactor):
 * - Dedicated task (priority: tskIDLE_PRIORITY + 2, stack: 4KB)
 * - Event-driven I/O with select() (0% CPU when idle)
 * - Thread-safe state access via std::atomic
 * - Keying events consumed via FreeRTOS queue (ISR-safe)
 * - Commands (Start/Stop/Config) via separate queue
 *
 * @see docs/plans/2025-11-15-remotecw-task-architecture-design.md
 */
class RemoteCwClient {
 public:
  RemoteCwClient();
  ~RemoteCwClient();

  RemoteCwClient(const RemoteCwClient&) = delete;
  RemoteCwClient& operator=(const RemoteCwClient&) = delete;

  /**
   * @brief Configure the client with server parameters and callbacks.
   * @note Creates FreeRTOS task and queues. Call once during initialization.
   */
  void Configure(const RemoteCwClientConfig& config, const RemoteCwClientCallbacks& callbacks);

  /**
   * @brief Start connection attempts (sends kStart command to task).
   * @return ESP_OK if command queued, ESP_ERR_TIMEOUT if queue full.
   */
  esp_err_t Start();

  /**
   * @brief Stop the client and close any active socket (sends kStop command to task).
   * @note Blocks until command accepted by task.
   */
  void Stop();

  /**
   * @brief Queue a keying event for transmission to remote server.
   * @param key_active true if CW key closed (transmitting), false if key open
   * @param timestamp_us Microsecond timestamp of keying state change
   * @return true if queued, false if queue full (event dropped)
   * @note Thread-safe, can be called from ISR context.
   */
  bool QueueKeyingEvent(bool key_active, int64_t timestamp_us);

  /**
   * @brief Retrieve the current state (thread-safe atomic read).
   */
  RemoteCwClientState GetState() const {
    return state_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Retrieve the most recent round-trip latency estimate in milliseconds (thread-safe).
   */
  uint32_t GetLatency() const {
    return measured_latency_ms_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Retrieve count of dropped keying events due to queue overflow (diagnostic).
   */
  uint32_t GetDroppedEventCount() const {
    return dropped_keying_events_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Dump diagnostic information about task status (for debugging).
   */
  void DumpDiagnostics() const;

  /**
   * @brief Set audio stream player for remote audio playback.
   * @param player Pointer to AudioStreamPlayer instance (non-owning).
   */
  void SetAudioStreamPlayer(audio::AudioStreamPlayer* player) {
    audio_stream_player_ = player;
  }

 private:
  //===========================================================================
  // Task Management
  //===========================================================================

  /**
   * @brief FreeRTOS task entry point (static, invoked by scheduler).
   * @param param Pointer to RemoteCwClient instance (this).
   */
  static void TaskFunction(void* param);

  /**
   * @brief Main task loop (never returns, infinite loop).
   */
  void RunTaskLoop();

  /**
   * @brief Process commands from main loop (non-blocking queue check).
   */
  void ProcessCommandQueue();

  /**
   * @brief Wait for command or timeout (used in Idle/Error states).
   * @param timeout_ms Timeout in milliseconds.
   */
  void WaitForCommandOrTimeout(uint32_t timeout_ms);

  /**
   * @brief Event-driven I/O handler for Handshake/Connected states.
   */
  void HandleConnectedState();

  /**
   * @brief Drain keying queue and encode MORSE frames to TX buffer.
   */
  void DrainKeyingQueue();

  /**
   * @brief Check periodic tasks (ping interval, handshake timeout, PTT tail).
   */
  void CheckPeriodicTasks();

  /**
   * @brief Transition to new state (atomic write, log transition).
   */
  void TransitionTo(RemoteCwClientState new_state);

  //===========================================================================
  // CWNet Protocol
  //===========================================================================

  enum class FrameCategory : uint8_t {
    kNoPayload = 0,
    kShortPayload,
    kLongPayload,
    kReserved,
  };

  static constexpr uint8_t kCmdMaskBlockLen = 0xC0;
  static constexpr uint8_t kCmdMaskCommand = 0x3F;
  static constexpr uint8_t kCmdMaskShort = 0x40;
  static constexpr uint8_t kCmdMaskLong = 0x80;
  static constexpr uint8_t kCmdConnect = 0x01;
  static constexpr uint8_t kCmdPing = 0x03;
  static constexpr uint8_t kCmdPrint = 0x04;
  static constexpr uint8_t kCmdTxInfo = 0x05;
  static constexpr uint8_t kCmdRigCtld = 0x06;
  static constexpr uint8_t kCmdMorse = 0x10;
  static constexpr uint8_t kCmdAudio = 0x11;
  static constexpr uint8_t kCmdVorbis = 0x12;
  static constexpr uint8_t kCmdCiV = 0x14;
  static constexpr uint8_t kCmdSpectrum = 0x15;
  static constexpr uint8_t kCmdFreqReport = 0x16;
  static constexpr uint8_t kCmdMeterReport = 0x20;
  static constexpr uint8_t kCmdPotiReport = 0x21;
  static constexpr uint8_t kCmdTunnel1 = 0x31;
  static constexpr uint8_t kCmdTunnel2 = 0x32;
  static constexpr uint8_t kCmdTunnel3 = 0x33;

  static constexpr size_t kRxBufferCapacity = 1024;   // Size chosen to hold several CW frames plus headers.
  static constexpr size_t kTxBufferCapacity = 1024;   // Matches RX capacity for symmetry and simplicity.
  static constexpr size_t kMaxKeyQueueDepth = 64;     // Allows buffering >7s at 9 events/sec worst case.
  static constexpr uint32_t kMaxTimestampMs = 1165;   // Protocol limit for 7-bit timestamp encoding.

  struct __attribute__((packed)) ConnectPayload {
    char username[44];
    char callsign[44];
    uint32_t permissions;
  };

  //===========================================================================
  // State Machine & Connection Management
  //===========================================================================

  void ScheduleReconnect(int64_t now_us);
  void AttemptResolution();
  void AttemptConnect(int64_t now_us);
  void EnterHandshake(int64_t now_us);

  void ResetConnectionState();
  void CloseSocket();

  //===========================================================================
  // Socket I/O
  //===========================================================================

  void HandleSocketRead();   // recv() and parse incoming frames
  void HandleSocketWrite();  // send() from TX buffer
  void DrainTxBuffer();

  //===========================================================================
  // CWNet Frame Handling
  //===========================================================================

  void PopulateConnectFrame();
  void SendPingRequest(int64_t now_us);
  void SendPttCommand(bool ptt_on);

  void ParseIncomingFrames(int64_t now_us);
  bool TryExtractFrame(size_t* frame_size, uint8_t* command, size_t* payload_offset,
                       size_t* payload_size) const;
  void HandleFrame(uint8_t command, const uint8_t* payload, size_t payload_size, int64_t now_us);
  void HandleConnectAck(const uint8_t* payload, size_t payload_size);
  void HandlePingFrame(const uint8_t* payload, size_t payload_size, int64_t now_us);
  void HandleMorseFrame(const uint8_t* payload, size_t payload_size, int64_t now_us);
  void HandlePrintFrame(const uint8_t* payload, size_t payload_size);
  void HandleAudioFrame(const uint8_t* payload, size_t payload_size);
  void HandleStubFrame(uint8_t command, const char* command_name);

  static uint8_t EncodeTimestamp(uint32_t milliseconds);
  static uint32_t DecodeTimestamp(uint8_t value);

  //===========================================================================
  // Member Variables
  //===========================================================================

  // Configuration and callbacks
  RemoteCwClientConfig config_{};
  RemoteCwClientCallbacks callbacks_{};

  // FreeRTOS task and queues
  TaskHandle_t task_handle_ = nullptr;
  QueueHandle_t keying_queue_ = nullptr;  // KeyingEvent queue (main loop → task)
  QueueHandle_t cmd_queue_ = nullptr;     // CommandMessage queue (main loop → task)

  // Atomic state (thread-safe reads from main loop, writes from task)
  std::atomic<RemoteCwClientState> state_{RemoteCwClientState::kIdle};
  std::atomic<uint32_t> measured_latency_ms_{0};
  std::atomic<uint32_t> dropped_keying_events_{0};  // Diagnostic counter

  // Socket and connection state (task-only access)
  int socket_fd_ = -1;
  int64_t state_enter_time_us_ = 0;
  int64_t next_reconnect_time_us_ = 0;

  struct sockaddr_storage resolved_addr_;
  socklen_t resolved_addr_len_ = 0;
  bool resolved_addr_valid_ = false;
  bool connect_in_progress_ = false;

  // RX/TX buffers (task-only access)
  uint8_t rx_buffer_[kRxBufferCapacity];
  size_t rx_bytes_ = 0;

  uint8_t tx_buffer_[kTxBufferCapacity];
  size_t tx_head_ = 0;
  size_t tx_tail_ = 0;

  // Keying timestamps (task-only access)
  int64_t last_local_key_timestamp_us_ = 0;
  int64_t last_remote_key_timestamp_us_ = 0;

  // Ping/latency measurement (task-only access)
  uint8_t pending_ping_id_ = 0;
  int64_t pending_ping_sent_us_ = 0;
  int64_t last_ping_time_us_ = 0;
  int32_t timer_sync_offset_ms_ = 0;  // Offset to sync our timer with server's timer

  // Connection state flags (task-only access)
  bool handshake_complete_ = false;
  bool connect_frame_sent_ = false;  // Track if CONNECT frame already populated (prevent duplicates)
  bool ptt_active_ = false;
  int64_t last_keying_activity_us_ = 0;

  audio::AudioStreamPlayer* audio_stream_player_ = nullptr;  // Injected dependency
};

}  // namespace remote
