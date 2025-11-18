/**
 * @file morse_decoder.cpp
 * @brief Main morse decoder facade implementation
 *
 * Implements state machine for morse code decoding with pattern accumulation
 * and circular buffer management.
 */

#include "morse_decoder/morse_decoder.hpp"

#include <algorithm>
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/high_precision_clock.hpp"

namespace morse_decoder {

namespace {
constexpr char kLogTag[] = "morse_decoder";

// Maximum pattern length per ITU standard
constexpr size_t kMaxPatternLength = 6;

// State machine state names for logging
const char* StateName(DecoderState state) {
  switch (state) {
    case DecoderState::kIdle: return "IDLE";
    case DecoderState::kReceiving: return "RECEIVING";
    case DecoderState::kCharGap: return "CHAR_GAP";
    case DecoderState::kWordGap: return "WORD_GAP";
    default: return "UNKNOWN";
  }
}

// KeyEvent names for logging
const char* EventName(KeyEvent event) {
  switch (event) {
    case KeyEvent::kDit: return "DIT";
    case KeyEvent::kDah: return "DAH";
    case KeyEvent::kIntraGap: return "INTRA_GAP";
    case KeyEvent::kCharGap: return "CHAR_GAP";
    case KeyEvent::kWordGap: return "WORD_GAP";
    case KeyEvent::kUnknown: return "UNKNOWN";
    default: return "INVALID";
  }
}

}  // namespace

// ============================================================================
// CONSTRUCTORS
// ============================================================================

MorseDecoder::MorseDecoder() : config_() {
  ESP_LOGI(kLogTag, "Morse decoder initialized (buffer_size=%zu, logging=%s)",
           config_.buffer_size, config_.enable_logging ? "enabled" : "disabled");
}

MorseDecoder::MorseDecoder(const MorseDecoderConfig& config) : config_(config) {
  ESP_LOGI(kLogTag, "Morse decoder initialized (buffer_size=%zu, logging=%s, classifier=%s)",
           config_.buffer_size,
           config_.enable_logging ? "enabled" : "disabled",
           config_.timing_classifier ? "linked" : "none");
}

// ============================================================================
// STATE MACHINE - ProcessEvent
// ============================================================================

void MorseDecoder::ProcessEvent(KeyEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Ignore events if decoder is disabled
  if (!enabled_) {
    return;
  }

  // Ignore kUnknown events (warm-up period)
  if (event == KeyEvent::kUnknown) {
    return;
  }

  // Update activity timestamp for inactivity timeout
  // Only update on dit/dah events (actual morse elements), not on gaps
  if (event == KeyEvent::kDit || event == KeyEvent::kDah) {
    last_activity_time_us_ = esp_timer_get_time();
  }

  if (config_.enable_logging) {
    ESP_LOGD(kLogTag, "ProcessEvent: state=%s, event=%s, pattern='%s'",
             StateName(state_), EventName(event), current_pattern_.c_str());
  }

  // State machine logic
  switch (state_) {
    case DecoderState::kIdle:
      // Waiting for first dit/dah to start pattern accumulation
      if (event == KeyEvent::kDit) {
        current_pattern_ += '.';
        state_ = DecoderState::kReceiving;
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → RECEIVING (started with dit)");
        }
      } else if (event == KeyEvent::kDah) {
        current_pattern_ += '-';
        state_ = DecoderState::kReceiving;
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → RECEIVING (started with dah)");
        }
      }
      // Ignore gaps in IDLE state (no pattern to finalize)
      break;

    case DecoderState::kReceiving:
      // Accumulating pattern (dits/dahs)
      if (event == KeyEvent::kDit) {
        current_pattern_ += '.';
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → pattern now: '%s'", current_pattern_.c_str());
        }

        // Pattern overflow protection: ITU max is 6 elements
        if (current_pattern_.length() > kMaxPatternLength) {
          ESP_LOGW(kLogTag, "Pattern overflow (>%zu): '%s' - finalizing as unknown",
                   kMaxPatternLength, current_pattern_.c_str());
          FinalizePattern(false);  // Finalize as '?'
        }
      } else if (event == KeyEvent::kDah) {
        current_pattern_ += '-';
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → pattern now: '%s'", current_pattern_.c_str());
        }

        // Pattern overflow protection
        if (current_pattern_.length() > kMaxPatternLength) {
          ESP_LOGW(kLogTag, "Pattern overflow (>%zu): '%s' - finalizing as unknown",
                   kMaxPatternLength, current_pattern_.c_str());
          FinalizePattern(false);
        }
      } else if (event == KeyEvent::kIntraGap) {
        // Intra-character gap: ignore (stay in RECEIVING)
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → intra-gap ignored (within character)");
        }

        // Emit gap marker event for timeline visualization
        if (timeline_logger_ != nullptr) {
          timeline::TimelineEvent gap_evt{
            .timestamp_us = esp_timer_get_time(),
            .type = timeline::EventType::kGapMarker,
            .arg0 = 0,  // 0 = element gap
            .arg1 = 0
          };
          timeline_logger_->push(gap_evt);
        }
      } else if (event == KeyEvent::kCharGap) {
        // Character boundary: finalize pattern
        state_ = DecoderState::kCharGap;
        FinalizePattern(false);  // No space
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → CHAR_GAP (character finalized)");
        }

        // Emit gap marker event for timeline visualization
        if (timeline_logger_ != nullptr) {
          timeline::TimelineEvent gap_evt{
            .timestamp_us = esp_timer_get_time(),
            .type = timeline::EventType::kGapMarker,
            .arg0 = 1,  // 1 = character gap
            .arg1 = 0
          };
          timeline_logger_->push(gap_evt);
        }
      } else if (event == KeyEvent::kWordGap) {
        // Word boundary: finalize pattern + add space
        state_ = DecoderState::kWordGap;
        FinalizePattern(true);  // Add space
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → WORD_GAP (word finalized + space)");
        }

        // Emit gap marker event for timeline visualization
        if (timeline_logger_ != nullptr) {
          timeline::TimelineEvent gap_evt{
            .timestamp_us = esp_timer_get_time(),
            .type = timeline::EventType::kGapMarker,
            .arg0 = 2,  // 2 = word gap
            .arg1 = 0
          };
          timeline_logger_->push(gap_evt);
        }
      }
      break;

    case DecoderState::kCharGap:
    case DecoderState::kWordGap:
      // After finalization, wait for next dit/dah to start new pattern
      if (event == KeyEvent::kDit) {
        current_pattern_ += '.';
        state_ = DecoderState::kReceiving;
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → RECEIVING (new pattern started with dit)");
        }
      } else if (event == KeyEvent::kDah) {
        current_pattern_ += '-';
        state_ = DecoderState::kReceiving;
        if (config_.enable_logging) {
          ESP_LOGD(kLogTag, "  → RECEIVING (new pattern started with dah)");
        }
      }
      // Ignore gaps after finalization (already processed)
      break;

    default:
      ESP_LOGE(kLogTag, "Invalid state: %d", static_cast<int>(state_));
      state_ = DecoderState::kIdle;
      current_pattern_.clear();
      break;
  }
}

// ============================================================================
// PATTERN FINALIZATION
// ============================================================================

void MorseDecoder::FinalizePattern(bool add_space) {
  // Caller must hold mutex_

  // Empty pattern: nothing to finalize (should not happen)
  if (current_pattern_.empty()) {
    ESP_LOGW(kLogTag, "FinalizePattern called with empty pattern");
    state_ = DecoderState::kIdle;
    return;
  }

  // Lookup pattern in morse table
  char decoded_char = morse_table_.Lookup(current_pattern_);

  if (decoded_char == '\0') {
    // Pattern not found: append '?' (unknown)
    decoded_char = '?';
    ESP_LOGW(kLogTag, "Unknown pattern: '%s' → '?'", current_pattern_.c_str());
  } else {
    if (config_.enable_logging) {
      ESP_LOGD(kLogTag, "Decoded: '%s' → '%c'", current_pattern_.c_str(), decoded_char);
    } else {
      // Always log decoded characters (not verbose)
      ESP_LOGI(kLogTag, "'%s' → '%c'", current_pattern_.c_str(), decoded_char);
    }
  }

  // TIMELINE: Emit decoded character event
  if (timeline_logger_) {
    timeline::TimelineEvent evt{};
    evt.timestamp_us = hal::HighPrecisionClock::NowMicros();
    evt.type = timeline::EventType::kDecodedChar;
    evt.arg0 = static_cast<int32_t>(decoded_char);  // ASCII character code
    evt.arg1 = 0;  // Unused
    timeline_logger_->push(evt);
  }

  // Append decoded character to buffer
  AppendToBuffer(decoded_char);
  last_char_ = decoded_char;

  // Add space if word boundary
  if (add_space) {
    AppendToBuffer(' ');
    if (config_.enable_logging) {
      ESP_LOGD(kLogTag, "Added space (word boundary)");
    }

    // TIMELINE: Emit space character event
    if (timeline_logger_) {
      timeline::TimelineEvent evt{};
      evt.timestamp_us = hal::HighPrecisionClock::NowMicros();
      evt.type = timeline::EventType::kDecodedChar;
      evt.arg0 = static_cast<int32_t>(' ');  // ASCII space
      evt.arg1 = 0;  // Unused
      timeline_logger_->push(evt);
    }
  }

  // Reset pattern and state
  current_pattern_.clear();
  state_ = DecoderState::kIdle;
}

void MorseDecoder::AppendToBuffer(char c) {
  // Caller must hold mutex_

  decoded_buffer_.push_back(c);

  // Circular buffer: discard oldest if exceeds buffer_size
  if (decoded_buffer_.size() > config_.buffer_size) {
    decoded_buffer_.pop_front();
    if (config_.enable_logging) {
      ESP_LOGD(kLogTag, "Buffer full (%zu), discarded oldest character", config_.buffer_size);
    }
  }
}

// ============================================================================
// GETTERS
// ============================================================================

std::string MorseDecoder::GetDecodedText() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Convert deque to string
  return std::string(decoded_buffer_.begin(), decoded_buffer_.end());
}

char MorseDecoder::GetLastChar() {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_char_;
}

std::string MorseDecoder::GetCurrentPattern() {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_pattern_;
}

uint32_t MorseDecoder::GetDetectedWPM() const {
  std::lock_guard<std::mutex> lock(mutex_);

  if (config_.timing_classifier != nullptr) {
    return config_.timing_classifier->GetWPM();
  }

  return 0;  // No classifier configured
}

DecoderState MorseDecoder::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

// ============================================================================
// CONTROL METHODS
// ============================================================================

void MorseDecoder::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);

  current_pattern_.clear();
  decoded_buffer_.clear();
  last_char_ = '\0';
  state_ = DecoderState::kIdle;

  ESP_LOGI(kLogTag, "Decoder reset (pattern + buffer cleared)");
}

void MorseDecoder::SetEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  enabled_ = enabled;

  ESP_LOGI(kLogTag, "Decoder %s", enabled ? "enabled" : "disabled");
}

bool MorseDecoder::IsEnabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return enabled_;
}

void MorseDecoder::Tick(int64_t now_us) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Only check timeout if decoder is enabled and actively receiving a pattern
  if (!enabled_ || state_ != DecoderState::kReceiving) {
    return;
  }

  // Calculate dynamic timeout based on detected WPM
  // Word gap is typically 7 dit units, so we use that as timeout
  int64_t timeout_us = kInactivityTimeoutUs;  // Default 500ms (word gap @ 20 WPM)

  if (config_.timing_classifier != nullptr) {
    const auto stats = config_.timing_classifier->GetTimingStats();
    if (stats.avg_dit_us > 0) {
      // Word gap threshold: 7 dit units
      timeout_us = stats.avg_dit_us * 7;
    }
  }

  // Check if we've been idle too long
  const int64_t elapsed_us = now_us - last_activity_time_us_;

  if (elapsed_us >= timeout_us) {
    // Force finalization of current pattern + add word space
    ESP_LOGI(kLogTag, "Inactivity timeout (%lld μs, threshold %lld μs) - finalizing pattern '%s' + space",
             elapsed_us, timeout_us, current_pattern_.c_str());
    FinalizePattern(true);  // Finalize with space (word boundary)
  }
}

void MorseDecoder::SetTimelineLogger(timeline::EventLogger<1024>* logger) {
  std::lock_guard<std::mutex> lock(mutex_);
  timeline_logger_ = logger;

  ESP_LOGI(kLogTag, "Timeline logger %s", logger ? "connected" : "disconnected");
}

}  // namespace morse_decoder
