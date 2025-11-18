/**
 * @file text_keyer.cpp
 * @brief Text-to-Morse keyer implementation
 */

#include "text_keyer/text_keyer.hpp"

#include "hal/tx_hal.hpp"
#include "audio_subsystem/audio_subsystem.hpp"
#include "morse_decoder/morse_encoder.hpp"
#include "esp_log.h"

namespace text_keyer {

namespace {
constexpr char kLogTag[] = "text_keyer";

// Timing constants (ITU standard)
// WPM = 1200 / dit_duration_ms
// dit = base unit
// dah = 3× dit
// intra-element gap = 1× dit
// character gap = 3× dit
// word gap = 7× dit
constexpr int kDahMultiplier = 3;
constexpr int kIntraGapMultiplier = 1;
constexpr int kCharGapMultiplier = 3;
constexpr int kWordGapMultiplier = 7;

}  // namespace

TextKeyer::TextKeyer() {
  encoder_ = new morse_decoder::MorseEncoder();
}

TextKeyer::~TextKeyer() {
  Abort();
  delete encoder_;
}

esp_err_t TextKeyer::Initialize(hal::TxHal* tx_hal, audio_subsystem::AudioSubsystem* audio_subsystem) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (tx_hal == nullptr) {
    ESP_LOGE(kLogTag, "Initialize failed: tx_hal is null");
    return ESP_ERR_INVALID_ARG;
  }

  tx_hal_ = tx_hal;
  audio_subsystem_ = audio_subsystem;

  ESP_LOGI(kLogTag, "TextKeyer initialized (speed=%u WPM)", speed_wpm_);
  return ESP_OK;
}

esp_err_t TextKeyer::SendText(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != KeyerState::kIdle) {
    ESP_LOGW(kLogTag, "SendText failed: already sending (state=%d)", static_cast<int>(state_));
    return ESP_ERR_INVALID_STATE;
  }

  if (text.empty()) {
    ESP_LOGW(kLogTag, "SendText failed: empty text");
    return ESP_ERR_INVALID_ARG;
  }

  // Encode text to morse patterns
  send_state_.original_text = text;
  send_state_.patterns = encoder_->EncodeText(text);
  send_state_.current_pattern_index = 0;
  send_state_.current_element_index = 0;
  send_state_.element_end_time_us = 0;
  send_state_.key_active = false;

  state_ = KeyerState::kSending;

  ESP_LOGI(kLogTag, "SendText started: '%s' (%zu patterns)", text.c_str(), send_state_.patterns.size());
  return ESP_OK;
}

void TextKeyer::Tick(int64_t now_us) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != KeyerState::kSending) {
    return;
  }

  // Check if current element is finished
  if (send_state_.element_end_time_us > 0 && now_us >= send_state_.element_end_time_us) {
    FinishCurrentElement(now_us);

    // Start next element
    if (!StartNextElement(now_us)) {
      // Sequence complete
      state_ = KeyerState::kIdle;
      ESP_LOGI(kLogTag, "SendText completed: '%s'", send_state_.original_text.c_str());
      send_state_ = SendState{};  // Reset state
    }
  } else if (send_state_.element_end_time_us == 0) {
    // First element - start immediately
    if (!StartNextElement(now_us)) {
      // Nothing to send (shouldn't happen if patterns is not empty)
      state_ = KeyerState::kIdle;
      send_state_ = SendState{};
    }
  }
}

void TextKeyer::Abort() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ == KeyerState::kIdle) {
    return;
  }

  // Unkey immediately
  SetKeyState(false);

  state_ = KeyerState::kIdle;
  send_state_ = SendState{};

  ESP_LOGI(kLogTag, "Transmission aborted");
}

void TextKeyer::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != KeyerState::kSending) {
    return;
  }

  // Unkey immediately
  SetKeyState(false);

  state_ = KeyerState::kPaused;
  ESP_LOGI(kLogTag, "Transmission paused");
}

void TextKeyer::Resume() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != KeyerState::kPaused) {
    return;
  }

  state_ = KeyerState::kSending;
  send_state_.element_end_time_us = 0;  // Restart timing
  ESP_LOGI(kLogTag, "Transmission resumed");
}

void TextKeyer::SetSpeed(uint32_t wpm) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (wpm < 5) {
    wpm = 5;
  } else if (wpm > 60) {
    wpm = 60;
  }

  speed_wpm_ = wpm;
  ESP_LOGI(kLogTag, "Speed set to %u WPM", wpm);
}

uint32_t TextKeyer::GetSpeed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return speed_wpm_;
}

bool TextKeyer::IsIdle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == KeyerState::kIdle;
}

bool TextKeyer::IsSending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == KeyerState::kSending;
}

KeyerState TextKeyer::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void TextKeyer::GetProgress(size_t& sent, size_t& total) const {
  std::lock_guard<std::mutex> lock(mutex_);
  sent = send_state_.current_pattern_index;
  total = send_state_.patterns.size();
}

int64_t TextKeyer::CalculateDitDurationUs(uint32_t wpm) const {
  // WPM = 1200 / dit_duration_ms
  // dit_duration_ms = 1200 / WPM
  // dit_duration_us = (1200 * 1000) / WPM
  return (1200LL * 1000LL) / static_cast<int64_t>(wpm);
}

bool TextKeyer::StartNextElement(int64_t now_us) {
  // Check if we've finished all patterns
  if (send_state_.current_pattern_index >= send_state_.patterns.size()) {
    return false;  // Sequence complete
  }

  const std::string& current_pattern = send_state_.patterns[send_state_.current_pattern_index];

  ESP_LOGD(kLogTag, "StartNextElement: pattern_idx=%zu/%zu, element_idx=%zu, pattern='%s'",
           send_state_.current_pattern_index, send_state_.patterns.size(),
           send_state_.current_element_index, current_pattern.c_str());

  // Handle space (word gap)
  if (current_pattern.empty()) {
    // Word gap (space between words)
    send_state_.current_element = ElementType::kWordGap;
    int64_t dit_us = CalculateDitDurationUs(speed_wpm_);
    send_state_.element_end_time_us = now_us + (dit_us * kWordGapMultiplier);
    send_state_.key_active = false;
    SetKeyState(false);

    // Move to next pattern
    send_state_.current_pattern_index++;
    send_state_.current_element_index = 0;

    ESP_LOGD(kLogTag, "Word gap started (%lld us)", dit_us * kWordGapMultiplier);
    return true;
  }

  // Check if we've finished current pattern (character)
  if (send_state_.current_element_index >= current_pattern.length()) {
    // Move to next pattern
    send_state_.current_pattern_index++;
    send_state_.current_element_index = 0;

    // Check if next pattern is a space (word gap) - if so, skip char gap
    // The word gap (7 units) REPLACES the char gap (3 units), not adds to it
    if (send_state_.current_pattern_index < send_state_.patterns.size() &&
        send_state_.patterns[send_state_.current_pattern_index].empty()) {
      // Next is space - skip char gap and go directly to word gap
      ESP_LOGD(kLogTag, "Skipping char gap before word gap");
      send_state_.element_end_time_us = 0;  // Start word gap immediately
      return true;
    }

    // Character gap (space between characters)
    send_state_.current_element = ElementType::kCharGap;
    int64_t dit_us = CalculateDitDurationUs(speed_wpm_);
    send_state_.element_end_time_us = now_us + (dit_us * kCharGapMultiplier);
    send_state_.key_active = false;
    SetKeyState(false);

    ESP_LOGD(kLogTag, "Char gap started (%lld us)", dit_us * kCharGapMultiplier);
    return true;
  }

  // Send dit or dah
  char element_char = current_pattern[send_state_.current_element_index];
  int64_t dit_us = CalculateDitDurationUs(speed_wpm_);
  int64_t element_duration_us;

  if (element_char == '.') {
    send_state_.current_element = ElementType::kDit;
    element_duration_us = dit_us;
    ESP_LOGD(kLogTag, "Dit started (%lld us)", element_duration_us);
  } else if (element_char == '-') {
    send_state_.current_element = ElementType::kDah;
    element_duration_us = dit_us * kDahMultiplier;
    ESP_LOGD(kLogTag, "Dah started (%lld us)", element_duration_us);
  } else {
    ESP_LOGE(kLogTag, "Invalid morse character '%c' in pattern", element_char);
    send_state_.current_element_index++;
    return StartNextElement(now_us);  // Skip invalid element
  }

  // Key transmitter
  send_state_.element_end_time_us = now_us + element_duration_us;
  send_state_.key_active = true;
  SetKeyState(true);

  send_state_.current_element_index++;

  return true;
}

void TextKeyer::FinishCurrentElement(int64_t now_us) {
  // Unkey if needed
  if (send_state_.key_active) {
    SetKeyState(false);
    send_state_.key_active = false;

    // Add intra-element gap if not at end of pattern
    if (send_state_.current_pattern_index < send_state_.patterns.size()) {
      const std::string& current_pattern = send_state_.patterns[send_state_.current_pattern_index];
      if (!current_pattern.empty() && send_state_.current_element_index < current_pattern.length()) {
        // Add intra-element gap
        int64_t dit_us = CalculateDitDurationUs(speed_wpm_);
        send_state_.element_end_time_us = now_us + (dit_us * kIntraGapMultiplier);
        send_state_.current_element = ElementType::kIntraGap;
        ESP_LOGD(kLogTag, "Intra gap started (%lld us)", dit_us * kIntraGapMultiplier);
        return;
      }
    }
  }

  // Reset element end time to trigger next element start
  send_state_.element_end_time_us = 0;
}

void TextKeyer::SetKeyState(bool active) {
  // Control TX HAL
  if (tx_hal_ != nullptr) {
    tx_hal_->SetActive(active);
  }

  // Control sidetone
  if (audio_subsystem_ != nullptr) {
    if (active) {
      audio_subsystem_->Start();
    } else {
      audio_subsystem_->Stop();
    }
  }

  ESP_LOGD(kLogTag, "Key state: %s", active ? "ACTIVE" : "INACTIVE");
}

}  // namespace text_keyer
