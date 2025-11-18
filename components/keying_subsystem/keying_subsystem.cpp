#include "keying_subsystem/keying_subsystem.hpp"

#include <algorithm>

#include "audio_subsystem/audio_subsystem.hpp"
#include "config/keying_presets.hpp"
#include "diagnostics_subsystem/diagnostics_subsystem.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "hal/high_precision_clock.hpp"
#include "hal/tx_hal.hpp"
#include "morse_decoder/adaptive_timing_classifier.hpp"
#include "morse_decoder/morse_decoder.hpp"
#include "remote/remote_cw_client.hpp"
#include "timeline/timeline_event_emitter.hpp"

namespace keying_subsystem {

namespace {
constexpr char kLogTag[] = "keying_subsystem";
}

KeyingSubsystem::KeyingSubsystem()
    : paddle_engine_(),
      paddle_callbacks_{},
      timeline_logger_(),
      paddle_event_queue_(nullptr),
      paddle_event_dropped_(0) {}

KeyingSubsystem::~KeyingSubsystem() {
  if (paddle_event_queue_ != nullptr) {
    vQueueDelete(paddle_event_queue_);
  }
}

keying::PaddleEngineConfig KeyingSubsystem::BuildEngineConfig(
    const config::DeviceConfig& device_config) {
  const config::KeyingConfig& keying_cfg = device_config.keying;

  // Helper to convert boolean memory flags to MemoryMode enum
  auto convert_memory_mode = [](bool enable_dit, bool enable_dah) -> keying::MemoryMode {
    if (enable_dit && enable_dah) return keying::MemoryMode::kDotAndDah;
    if (enable_dit) return keying::MemoryMode::kDotOnly;
    if (enable_dah) return keying::MemoryMode::kDahOnly;
    return keying::MemoryMode::kNone;
  };

  // Helper to convert latch mode to squeeze mode
  // State latch (live update) → kLive, Edge trigger (snapshot) → kSnapshot
  auto convert_squeeze_mode = [](bool use_state_latch) -> keying::SqueezeMode {
    return use_state_latch ? keying::SqueezeMode::kLive : keying::SqueezeMode::kSnapshot;
  };

  // Start with base configuration from device config
  keying::PaddleEngineConfig engine_config{};
  engine_config.speed_wpm = keying_cfg.speed_wpm;
  engine_config.iambic_mode = keying::IambicMode::kB;  // Default to Mode B

  // L-S-P timing parameters (Task 3.6: Hot-reload support)
  engine_config.timing_l = keying_cfg.timing_l;
  engine_config.timing_s = keying_cfg.timing_s;
  engine_config.timing_p = keying_cfg.timing_p;

  // Memory window percentages: both are positions from element start
  // - memory_open_percent: window opens at this % (0 = opens immediately)
  // - memory_close_percent: window closes at this % (100 = closes at element end)
  engine_config.mem_block_start_pct = std::clamp(keying_cfg.memory_open_percent, 0.0f, 100.0f);
  engine_config.mem_block_end_pct = std::clamp(keying_cfg.memory_close_percent, 0.0f, 100.0f);

  // For manual preset, use manual configuration flags directly
  if (keying_cfg.preset == config::KeyingPreset::kManual) {
    engine_config.memory_mode = convert_memory_mode(
        keying_cfg.manual_memory_enable_dit, keying_cfg.manual_memory_enable_dah);
    engine_config.squeeze_mode = convert_squeeze_mode(keying_cfg.manual_use_state_latch);
    return engine_config;
  }

  // For preset modes (V0-V9), load preset configuration
  // Validate preset is in valid range (0-9)
  const auto preset_index = static_cast<size_t>(keying_cfg.preset);
  if (preset_index < 10) {
    // Task 3.3: Pass device_config to GetPresetConfig() for user customization support
    const config::PresetConfigData& preset = config::GetPresetConfig(keying_cfg.preset, &device_config);
    engine_config.mem_block_start_pct = std::clamp(preset.memory_open_percent, 0.0f, 100.0f);
    engine_config.mem_block_end_pct = std::clamp(preset.memory_close_percent, 0.0f, 100.0f);
    engine_config.memory_mode = convert_memory_mode(preset.enable_dit_memory, preset.enable_dah_memory);
    engine_config.squeeze_mode = convert_squeeze_mode(preset.use_state_latch);
  } else {
    // Invalid preset - fall back to safe defaults (Accukeyer Both behavior)
    ESP_LOGW(kLogTag, "Invalid preset %d, using Accukeyer defaults",
             static_cast<int>(keying_cfg.preset));
    engine_config.mem_block_start_pct = 60.0f;
    engine_config.mem_block_end_pct = 99.0f;  // Window closes at 99% (late release window)
    engine_config.memory_mode = keying::MemoryMode::kDotAndDah;
    engine_config.squeeze_mode = keying::SqueezeMode::kLive;  // State latch
  }

  return engine_config;
}

void KeyingSubsystem::ApplyConfig(const config::DeviceConfig& device_config) {
  ESP_LOGI(kLogTag, "Applying runtime config changes");

  // Rebuild engine config from new device config
  keying::PaddleEngineConfig engine_config = BuildEngineConfig(device_config);

  // Task 3.5: Warn if L-S-P timing deviates significantly from standard (30-50-50)
  const uint8_t L = engine_config.timing_l;
  const uint8_t S = engine_config.timing_s;
  const uint8_t P = engine_config.timing_p;
  const bool l_deviated = (L < 20) || (L > 40);  // |L-30| > 10
  const bool s_deviated = (S < 30) || (S > 70);  // |S-50| > 20
  const bool p_deviated = (P < 30) || (P > 70);  // |P-50| > 20
  if (l_deviated || s_deviated || p_deviated) {
    ESP_LOGW(kLogTag, "L-S-P timing deviates from standard (30-50-50): L=%u S=%u P=%u "
             "(dash=%.1f:1, effective WPM will differ from nominal)",
             L, S, P, static_cast<float>(L) / 10.0f);
  }

  // Reinitialize paddle engine with new config (callbacks remain the same)
  paddle_engine_.Initialize(engine_config, paddle_callbacks_);

  ESP_LOGI(kLogTag, "Keying config applied: speed=%" PRIu32 " WPM, L-S-P=%u-%u-%u, preset=%d",
           engine_config.speed_wpm, L, S, P, static_cast<int>(device_config.keying.preset));
}

void IRAM_ATTR KeyingSubsystem::RecordPaddleEvent(const hal::PaddleEvent& event, void* context) {
  auto* subsystem = static_cast<KeyingSubsystem*>(context);
  if (subsystem == nullptr) {
    return;
  }

  // Enqueue event to FreeRTOS queue (ISR-safe).
  // NOTE: paddle_event_queue_ is guaranteed to exist because KeyingSubsystemPhase
  // (creates queue) runs before PaddleHalPhase (enables ISR) in init sequence.
  BaseType_t higher_priority_task_woken = pdFALSE;
  if (xQueueSendFromISR(subsystem->paddle_event_queue_, &event, &higher_priority_task_woken) !=
      pdTRUE) {
    subsystem->paddle_event_dropped_.fetch_add(1, std::memory_order_relaxed);
  }
  portYIELD_FROM_ISR(higher_priority_task_woken);
}

void KeyingSubsystem::HandleKeyingElementStarted(keying::PaddleElement element,
                                                 int64_t start_time_us, void* context) {
  auto* subsystem = static_cast<KeyingSubsystem*>(context);
  if (subsystem == nullptr) {
    return;
  }
  timeline::TimelineEvent evt{
      .timestamp_us = (start_time_us != 0) ? start_time_us : hal::HighPrecisionClock::NowMicros(),
      .type = timeline::EventType::kKeying,
      .arg0 = static_cast<uint32_t>(element),
      .arg1 = 1,
  };
  subsystem->timeline_logger_.push(evt);
}

void KeyingSubsystem::HandleKeyingElementFinished(keying::PaddleElement element,
                                                  int64_t end_time_us, void* context) {
  auto* subsystem = static_cast<KeyingSubsystem*>(context);
  if (subsystem == nullptr) {
    return;
  }
  timeline::TimelineEvent evt{
      .timestamp_us = (end_time_us != 0) ? end_time_us : hal::HighPrecisionClock::NowMicros(),
      .type = timeline::EventType::kKeying,
      .arg0 = static_cast<uint32_t>(element),
      .arg1 = 0,
  };
  subsystem->timeline_logger_.push(evt);
}

void KeyingSubsystem::HandleKeyingStateChanged(bool key_active, int64_t timestamp_us,
                                               void* context) {
  auto* subsystem = static_cast<KeyingSubsystem*>(context);
  if (subsystem == nullptr) {
    ESP_LOGE(kLogTag, "HandleKeyingStateChanged called with NULL context!");
    return;
  }

  // ═══ CRITICAL: Audio Start()/Stop() MUST be called FIRST ═══
  // Reason: ESP_LOGI on UART can take 20-60ms, introducing unacceptable latency
  // Audio timing is critical for morse code, so call Start()/Stop() immediately
  // BEFORE any logging or other operations that could delay execution

  // Control sidetone (synchronized with TX) - FIRST, before any logging
  if (subsystem->audio_subsystem_ != nullptr) {
    if (key_active) {
      subsystem->audio_subsystem_->Start();
    } else {
      subsystem->audio_subsystem_->Stop();
    }
  }

  // Control TX output GPIO - also time-critical
  if (subsystem->tx_hal_ != nullptr) {
    subsystem->tx_hal_->SetActive(key_active);
  }

  // Now safe to log (after time-critical operations completed)
  const int64_t now_us = hal::HighPrecisionClock::NowMicros();
  const int64_t callback_latency_us = now_us - timestamp_us;

  ESP_LOGI(kLogTag, "═══ AUDIO %s @ %lld us (event_time=%lld, latency=%lld us) ═══",
           key_active ? "START" : "STOP",
           now_us,
           timestamp_us,
           callback_latency_us);

  // Queue local key event to remote client (if connected)
  if (subsystem->remote_client_ != nullptr) {
    const auto state = subsystem->remote_client_->GetState();
    ESP_LOGI(kLogTag, "Remote client state check: state=%d (kConnected=%d)",
             static_cast<int>(state), static_cast<int>(remote::RemoteCwClientState::kConnected));
    if (state == remote::RemoteCwClientState::kConnected) {
      ESP_LOGI(kLogTag, "Attempting to queue keying event to remote client...");
      if (!subsystem->remote_client_->QueueKeyingEvent(key_active, timestamp_us)) {
        ESP_LOGW(kLogTag, "Remote client key queue full, event dropped");
      }

      // PTT management: activate PTT on first key-down, extend timeout on any key activity
      if (key_active) {
        // Key down: ensure we're in TX mode
        if (subsystem->audio_subsystem_ != nullptr &&
            !subsystem->audio_subsystem_->IsModeTX()) {
          subsystem->audio_subsystem_->SetModeTX();
        }

        // Activate PTT if not already active
        if (!subsystem->ptt_active_) {
          subsystem->ptt_active_ = true;
          // Note: Actual PTT control would go here (e.g., GPIO pin).
          // For now, PTT is logical state only.
          ESP_LOGD(kLogTag, "Remote PTT activated");
        }

        // Update PTT timeout: base tail + current measured latency
        const uint32_t dynamic_tail_ms = subsystem->ptt_tail_ms_ +
                                         subsystem->remote_client_->GetLatency();
        subsystem->ptt_timeout_us_ = timestamp_us + (dynamic_tail_ms * 1000);
      } else {
        // Key up: update PTT timeout
        if (subsystem->ptt_active_) {
          const uint32_t dynamic_tail_ms = subsystem->ptt_tail_ms_ +
                                           subsystem->remote_client_->GetLatency();
          subsystem->ptt_timeout_us_ = timestamp_us + (dynamic_tail_ms * 1000);
        }
      }
    } else {
      ESP_LOGI(kLogTag, "Remote client NOT connected (state=%d), keying event NOT queued",
               static_cast<int>(state));
    }
  } else {
    ESP_LOGI(kLogTag, "Remote client is NULL, keying event NOT queued");
  }

  // Feed timing classifier for morse decoder (if enabled)
  if (subsystem->timing_classifier_ != nullptr) {
    // Track previous timestamp for duration calculation
    static int64_t last_timestamp_us = 0;

    // Calculate duration since last event
    if (last_timestamp_us > 0) {
      const int64_t duration_us = timestamp_us - last_timestamp_us;
      if (duration_us > 0) {
        // Classify the duration that just ended:
        // - If current state is key_active=true: previous duration was key-off (gap)
        // - If current state is key_active=false: previous duration was key-on (dit/dah)
        const bool was_key_on = !key_active;  // What just ended
        morse_decoder::KeyEvent event =
            subsystem->timing_classifier_->ClassifyDuration(duration_us, was_key_on);

        // Feed classified event to morse decoder (Phase 2)
        if (subsystem->morse_decoder_ != nullptr) {
          subsystem->morse_decoder_->ProcessEvent(event);
        }
      }
    }

    // Update timestamp tracker
    last_timestamp_us = timestamp_us;
  }
}

esp_err_t KeyingSubsystem::Initialize(const config::DeviceConfig& device_config) {
  ESP_LOGI(kLogTag, "Initializing keying subsystem");

  // Create FreeRTOS paddle event queue.
  paddle_event_queue_ = xQueueCreate(kPaddleEventQueueCapacity, sizeof(hal::PaddleEvent));
  if (paddle_event_queue_ == nullptr) {
    ESP_LOGE(kLogTag, "Failed to create paddle event queue");
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kLogTag, "Paddle event queue created (capacity=%u)", kPaddleEventQueueCapacity);

  // Configure paddle engine callbacks (context = this KeyingSubsystem instance).
  // IMPORTANT: Preserve timeline_hooks if they were set by SetTimelineEmitter() before Initialize().
  // SetTimelineEmitter() is called in SubsystemCreationPhase (Phase 8), before KeyingSubsystemPhase (Phase 11).
  auto saved_timeline_hooks = paddle_callbacks_.timeline_hooks;

  paddle_callbacks_ = {
      .on_element_started = HandleKeyingElementStarted,
      .on_element_finished = HandleKeyingElementFinished,
      .on_key_state_changed = HandleKeyingStateChanged,
      .context = this,
      .timeline_hooks = saved_timeline_hooks,  // Restore timeline hooks
  };

  // Initialize paddle engine.
  const keying::PaddleEngineConfig engine_config = BuildEngineConfig(device_config);
  if (!paddle_engine_.Initialize(engine_config, paddle_callbacks_)) {
    ESP_LOGE(kLogTag, "Paddle engine initialization failed");
    return ESP_FAIL;
  }

  ESP_LOGI(kLogTag, "Keying subsystem initialized (speed=%u WPM)", 
           static_cast<unsigned int>(engine_config.speed_wpm));
  return ESP_OK;
}

void KeyingSubsystem::DrainPaddleEvents() {
  hal::PaddleEvent event;
  static uint32_t last_dropped_count = 0;

  while (xQueueReceive(paddle_event_queue_, &event, 0) == pdTRUE) {
    // Update diagnostics (LED visualization) - moved from ISR to task context
    if (diagnostics_subsystem_ != nullptr) {
      diagnostics_subsystem_->UpdatePaddleActivity(event.line, event.active, event.timestamp_us);
    }

    paddle_engine_.OnPaddleEvent(event);

    // Log paddle event to timeline (moved out of ISR context).
    timeline::TimelineEvent timeline_evt{
        .timestamp_us = event.timestamp_us,
        .type = timeline::EventType::kPaddleEdge,
        .arg0 = static_cast<uint32_t>(event.line),
        .arg1 = event.active ? 1U : 0U,
    };
    timeline_logger_.push(timeline_evt);
  }

  // Check for dropped events (queue overflow)
  const uint32_t current_dropped = GetDroppedEventCount();
  if (current_dropped != last_dropped_count) {
    ESP_LOGW(kLogTag, "ISR queue overflow! Dropped events: %" PRIu32 " (+%" PRIu32 ")",
             current_dropped, current_dropped - last_dropped_count);
    last_dropped_count = current_dropped;
  }
}

void KeyingSubsystem::TickRemotePtt(int64_t now_us) {
  if (!ptt_active_) {
    return;
  }

  if (now_us >= ptt_timeout_us_) {
    // PTT timeout expired: switch audio mode based on remote client state
    ptt_active_ = false;
    ESP_LOGD(kLogTag, "Remote PTT deactivated (tail timeout)");

    if (audio_subsystem_ != nullptr && remote_client_ != nullptr) {
      if (remote_client_->GetState() == remote::RemoteCwClientState::kConnected) {
        // Remote still connected: switch to RX mode to receive remote audio stream
        audio_subsystem_->SetModeRX();
      } else {
        // Remote disconnected/stopped: restore TX mode for local sidetone
        audio_subsystem_->SetModeTX();
      }
    }
  }
}

void KeyingSubsystem::Tick(int64_t now_us) {
  paddle_engine_.Tick(now_us);
  TickRemotePtt(now_us);

  // Tick morse decoder for inactivity timeout handling
  if (morse_decoder_ != nullptr) {
    morse_decoder_->Tick(now_us);
  }
}

void KeyingSubsystem::SetTimelineEmitter(timeline::TimelineEventEmitter* emitter) {
  timeline_emitter_ = emitter;

  if (timeline_emitter_ != nullptr) {
    // Get hooks from emitter and forward to paddle engine callbacks
    paddle_callbacks_.timeline_hooks = timeline_emitter_->GetHooks();

    ESP_LOGI(kLogTag, "Timeline event emitter wired (hooks ready for next engine initialization)");
  } else {
    // Clear timeline hooks if emitter is removed
    paddle_callbacks_.timeline_hooks = timeline::TimelineHooks{};
    ESP_LOGI(kLogTag, "Timeline event emitter disconnected");
  }

  // Note: The updated callbacks will be used when the paddle engine is initialized/reinitialized.
  // This method should be called during subsystem wiring phase (before Initialize() is called).
  // If called after initialization, the hooks will take effect on the next ApplyConfig().
}

void KeyingSubsystem::DumpTimelineHooksStatus() const {
  ESP_LOGI(kLogTag, "=== Timeline Hooks Status ===");
  ESP_LOGI(kLogTag, "Timeline Emitter: %s (%p)",
           timeline_emitter_ ? "CONNECTED" : "NULL", timeline_emitter_);
  ESP_LOGI(kLogTag, "");
  ESP_LOGI(kLogTag, "Paddle Callbacks (in paddle_callbacks_ struct):");
  ESP_LOGI(kLogTag, "  OnMemoryWindowChanged: %s (%p)",
           paddle_callbacks_.timeline_hooks.OnMemoryWindowChanged ? "SET" : "NULL",
           paddle_callbacks_.timeline_hooks.OnMemoryWindowChanged);
  ESP_LOGI(kLogTag, "  OnLatchStateChanged:   %s (%p)",
           paddle_callbacks_.timeline_hooks.OnLatchStateChanged ? "SET" : "NULL",
           paddle_callbacks_.timeline_hooks.OnLatchStateChanged);
  ESP_LOGI(kLogTag, "  OnSqueezeDetected:     %s (%p)",
           paddle_callbacks_.timeline_hooks.OnSqueezeDetected ? "SET" : "NULL",
           paddle_callbacks_.timeline_hooks.OnSqueezeDetected);
  ESP_LOGI(kLogTag, "  Context pointer:       %p",
           paddle_callbacks_.timeline_hooks.context);
  ESP_LOGI(kLogTag, "");
  ESP_LOGI(kLogTag, "Note: Hooks are copied to PaddleEngine during Initialize().");
  ESP_LOGI(kLogTag, "If hooks show NULL, check:");
  ESP_LOGI(kLogTag, "  1. SetTimelineEmitter() was called before Initialize()");
  ESP_LOGI(kLogTag, "  2. TimelineEventEmitter::GetHooks() returns valid pointers");
  ESP_LOGI(kLogTag, "  3. PaddleEngine::Initialize() received and copied callbacks");
  ESP_LOGI(kLogTag, "===========================");
}

}  // namespace keying_subsystem
