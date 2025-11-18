#pragma once

/**
 * @file keying_subsystem.hpp
 * @brief Keying Subsystem - Integration layer for paddle input and CW engine
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * This component is the **system integration layer** that connects hardware/OS
 * concerns (ISR, FreeRTOS queues, timeline logging) with the pure CW logic.
 *
 * SEPARATION OF CONCERNS:
 * - keying::PaddleEngine   = Pure CW algorithm (iambic logic, timing, testable on host)
 * - KeyingSubsystem        = OS integration (ISR callbacks, FreeRTOS queues, timeline)
 *
 * WHY THE SEPARATION:
 * 1. **Testability**: PaddleEngine can be unit-tested on host without FreeRTOS/ESP-IDF
 * 2. **Reusability**: PaddleEngine could be used in other contexts (remote keyer, simulator)
 * 3. **Maintainability**: Algorithm changes don't affect queue/ISR management and vice versa
 *
 * RESPONSIBILITIES:
 * - Create/manage FreeRTOS queue for paddle events (ISR → main loop deferred processing)
 * - Provide RecordPaddleEvent() ISR callback (registered with PaddleHal)
 * - Initialize and configure PaddleEngine from DeviceConfig
 * - Log paddle events and keying elements to timeline::EventLogger
 * - Expose engine reference for runtime config updates (console commands)
 *
 * USAGE PATTERN:
 * ```
 * KeyingSubsystem keying;
 * keying.Initialize(device_config);          // Setup queue + engine + callbacks
 * paddle_hal.Initialize(config, KeyingSubsystem::RecordPaddleEvent, &keying);
 *
 * // Main loop:
 * keying.DrainPaddleEvents();  // Process ISR queue → engine
 * keying.Tick(now_us);         // Update engine state → emit keying elements
 * ```
 */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hal/paddle_hal.hpp"
#include "keying/paddle_engine.hpp"
#include "timeline/event_logger.hpp"
#include "config/device_config.hpp"

#include <atomic>
#include <cstddef>

// Forward declarations to avoid circular dependencies
namespace hal {
class TxHal;
}

namespace audio_subsystem {
class AudioSubsystem;
}

namespace remote {
class RemoteCwClient;
}

namespace diagnostics_subsystem {
class DiagnosticsSubsystem;
}

namespace morse_decoder {
class AdaptiveTimingClassifier;
class MorseDecoder;
}

namespace timeline {
class TimelineEventEmitter;
}

namespace keying_subsystem {

/**
 * @brief Manages paddle input processing, keying engine, and timeline logging.
 *
 * Responsibilities:
 * - ISR-based paddle event capture and queuing
 * - Paddle engine lifecycle (config, initialize, tick)
 * - Timeline event logging for paddle events and keying elements
 * - Thread-safe event deferred processing (ISR → main loop)
 */
class KeyingSubsystem {
 public:
  // Paddle event queue capacity - sized for mechanical bounce + ISR bursts
  // Rationale: Mechanical paddle contacts can bounce 1-10ms during transitions, generating
  // 100-300+ ISR events per physical press (GPIO_INTR_ANYEDGE on both rising/falling edges).
  // DrainPaddleEvents() runs at 1kHz (1ms interval), so worst-case buffering during one
  // main loop iteration: 3-6 events per paddle bounce (mechanical bounce 1-10ms).
  // Capacity: 256 slots provides headroom for multiple overlapping bounces (dit+dah pressed
  // simultaneously) plus transient scheduling delays when serial_console monopolizes CPU.
  // If queue overflows, dropped event counter increments (monitored via GetDroppedEventCount()).
  // Memory cost: 256 * sizeof(PaddleEvent) = 256 * 16 bytes = 4KB RAM.
  static constexpr size_t kPaddleEventQueueCapacity = 256;

  // Timeline event capacity - sized for session recording and diagnostics export
  // Rationale: Stores complete event history (paddle edges + keying elements) for analysis.
  // At 80 WPM sustained: ~8.8 paddle events/sec + ~4.4 keying events/sec ≈ 13 events/sec.
  // 1024 slots = 78 seconds of continuous operation before ring buffer wraps.
  // Sufficient for capturing error sequences and exporting to analysis tools.
  static constexpr size_t kTimelineCapacity = 1024;

  KeyingSubsystem();
  ~KeyingSubsystem();

  /**
   * @brief Initialize paddle engine, timeline logger, and event queue.
   * @param device_config Device configuration for keying parameters.
   * @return ESP_OK on success, error code otherwise.
   */
  esp_err_t Initialize(const config::DeviceConfig& device_config);

  /**
   * @brief Process queued paddle events (call from main loop).
   */
  void DrainPaddleEvents();

  /**
   * @brief Update paddle engine state (call from main loop).
   * @param now_us Current timestamp in microseconds.
   */
  void Tick(int64_t now_us);

  /**
   * @brief Get direct reference to paddle engine for config updates.
   */
  keying::PaddleEngine& GetEngine() { return paddle_engine_; }

  /**
   * @brief Get timeline logger for diagnostics/export.
   */
  timeline::EventLogger<kTimelineCapacity>& GetTimeline() { return timeline_logger_; }

  /**
   * @brief Get number of dropped paddle events (queue overflow).
   */
  uint32_t GetDroppedEventCount() const {
    return paddle_event_dropped_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Debug: Dump complete paddle engine state (for troubleshooting lockups).
   */
  void DumpEngineState(int64_t now_us) const {
    paddle_engine_.DumpState(now_us);
  }

  /**
   * @brief ISR callback for paddle events (registered with PaddleHal).
   */
  static void IRAM_ATTR RecordPaddleEvent(const hal::PaddleEvent& event, void* context);

  /**
   * @brief Build paddle engine config from device config (public for console commands).
   */
  static keying::PaddleEngineConfig BuildEngineConfig(const config::DeviceConfig& device_config);

  /**
   * @brief Apply runtime configuration changes (hot-reload).
   *
   * Rebuilds engine config from device config and reinitializes paddle engine.
   * Safe to call while engine is running. Only affects runtime-changeable parameters:
   * - speed_wpm
   * - preset (V0-V9, manual)
   * - memory_open_percent, memory_close_percent
   * - consider_late_release
   * - manual_* settings
   *
   * @param device_config Updated device configuration
   */
  void ApplyConfig(const config::DeviceConfig& device_config);

  /**
   * @brief Set TX HAL reference (injected from ApplicationController).
   * @param tx_hal Pointer to TxHal instance (non-owning)
   */
  void SetTxHal(hal::TxHal* tx_hal) { tx_hal_ = tx_hal; }

  /**
   * @brief Set Audio Subsystem reference (injected from ApplicationController).
   * @param audio_subsystem Pointer to AudioSubsystem instance (non-owning)
   */
  void SetAudioSubsystem(audio_subsystem::AudioSubsystem* audio_subsystem) {
    audio_subsystem_ = audio_subsystem;
  }

  /**
   * @brief Set Remote CW Client reference (injected from ApplicationController).
   * @param remote_client Pointer to RemoteCwClient instance (non-owning)
   * @param ptt_tail_ms Base PTT tail delay in milliseconds (latency will be added dynamically)
   */
  void SetRemoteClient(remote::RemoteCwClient* remote_client, uint32_t ptt_tail_ms) {
    remote_client_ = remote_client;
    ptt_tail_ms_ = ptt_tail_ms;
  }

  /**
   * @brief Set Diagnostics Subsystem reference (injected from ApplicationController).
   * @param diagnostics_subsystem Pointer to DiagnosticsSubsystem instance (non-owning)
   *
   * Required for paddle activity LED visualization. UpdatePaddleActivity() will be
   * called from DrainPaddleEvents() in task context (not ISR).
   */
  void SetDiagnosticsSubsystem(diagnostics_subsystem::DiagnosticsSubsystem* diagnostics_subsystem) {
    diagnostics_subsystem_ = diagnostics_subsystem;
  }

  /**
   * @brief Set Morse Decoder Timing Classifier reference (injected from ApplicationController).
   * @param timing_classifier Pointer to AdaptiveTimingClassifier instance (non-owning)
   *
   * The timing classifier receives all keying state changes and adaptively learns
   * dit/dah timing parameters. Used by full morse decoder in Phase 2.
   */
  void SetTimingClassifier(morse_decoder::AdaptiveTimingClassifier* timing_classifier) {
    timing_classifier_ = timing_classifier;
  }

  /**
   * @brief Get Timing Classifier reference.
   * @return Pointer to timing classifier (may be nullptr)
   */
  morse_decoder::AdaptiveTimingClassifier* GetTimingClassifier() const {
    return timing_classifier_;
  }

  /**
   * @brief Set morse decoder for real-time keying decoding (Phase 2)
   * @param morse_decoder Morse decoder instance (non-owning pointer)
   *
   * The morse decoder receives classified keying events from the timing
   * classifier and decodes them to text in real-time.
   */
  void SetMorseDecoder(morse_decoder::MorseDecoder* morse_decoder) {
    morse_decoder_ = morse_decoder;
  }

  /**
   * @brief Get Morse Decoder reference.
   * @return Pointer to morse decoder (may be nullptr)
   */
  morse_decoder::MorseDecoder* GetMorseDecoder() const {
    return morse_decoder_;
  }

  /**
   * @brief Set Timeline Event Emitter reference (injected from ApplicationController).
   * @param emitter Pointer to TimelineEventEmitter instance (non-owning)
   *
   * The timeline event emitter receives hook callbacks from the paddle engine
   * and emits timeline events for advanced features (memory window, late release,
   * latch, squeeze detection). Used for real-time visualization and diagnostics.
   */
  void SetTimelineEmitter(timeline::TimelineEventEmitter* emitter);

  /**
   * @brief Dump timeline hooks status for debugging
   *
   * Prints current timeline hooks state (enabled/disabled, function pointers).
   * Useful for troubleshooting missing LOGIC overlay events in timeline UI.
   */
  void DumpTimelineHooksStatus() const;

 private:

  /**
   * @brief Callback: keying element started (logs to timeline).
   */
  static void HandleKeyingElementStarted(keying::PaddleElement element, int64_t start_time_us,
                                         void* context);

  /**
   * @brief Callback: keying element finished (logs to timeline).
   */
  static void HandleKeyingElementFinished(keying::PaddleElement element, int64_t end_time_us,
                                          void* context);

  /**
   * @brief Callback: key state changed (placeholder for remote protocol).
   */
  static void HandleKeyingStateChanged(bool key_active, int64_t timestamp_us, void* context);

  /**
   * @brief Tick remote PTT management (call from Tick() main loop).
   * @param now_us Current timestamp in microseconds.
   */
  void TickRemotePtt(int64_t now_us);

  keying::PaddleEngine paddle_engine_;
  keying::PaddleEngineCallbacks paddle_callbacks_;
  timeline::EventLogger<kTimelineCapacity> timeline_logger_;
  QueueHandle_t paddle_event_queue_;
  std::atomic<uint32_t> paddle_event_dropped_;

  // Injected dependencies (non-owning pointers)
  hal::TxHal* tx_hal_ = nullptr;
  audio_subsystem::AudioSubsystem* audio_subsystem_ = nullptr;
  remote::RemoteCwClient* remote_client_ = nullptr;
  diagnostics_subsystem::DiagnosticsSubsystem* diagnostics_subsystem_ = nullptr;
  morse_decoder::AdaptiveTimingClassifier* timing_classifier_ = nullptr;
  morse_decoder::MorseDecoder* morse_decoder_ = nullptr;
  timeline::TimelineEventEmitter* timeline_emitter_ = nullptr;

  // Remote PTT state
  bool ptt_active_ = false;
  int64_t ptt_timeout_us_ = 0;
  uint32_t ptt_tail_ms_ = 200;  // Base PTT tail (200ms default per requirements)
};

}  // namespace keying_subsystem
