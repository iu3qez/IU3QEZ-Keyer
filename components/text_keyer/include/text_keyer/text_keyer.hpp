/**
 * @file text_keyer.hpp
 * @brief Text-to-Morse keyer - sends text as CW
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * Converts text strings to morse code and keys transmitter with proper timing.
 * Complements paddle input by providing keyboard/stored message functionality.
 *
 * RESPONSIBILITIES:
 * - Encode text to morse patterns using MorseEncoder
 * - Generate precise timing for dit/dah/gaps based on WPM
 * - Control TX HAL and sidetone during transmission
 * - Support pause/resume and abort operations
 * - Thread-safe state management for concurrent access
 *
 * TIMING CALCULATIONS:
 * ====================
 * ITU "PARIS" standard: 50 dit-lengths per word at standard spacing
 * WPM = 1200 / dit_duration_ms
 *
 * At 20 WPM:
 *   dit = 60 ms
 *   dah = 180 ms (3× dit)
 *   intra-element gap = 60 ms (1× dit, between dit/dah in same character)
 *   character gap = 180 ms (3× dit, between characters)
 *   word gap = 420 ms (7× dit, between words)
 *
 * USAGE PATTERN:
 * ==============
 * TextKeyer keyer;
 * keyer.Initialize(tx_hal, audio_subsystem);
 * keyer.SetSpeed(20);  // 20 WPM
 *
 * keyer.SendText("CQ CQ CQ DE IU3QEZ");
 *
 * while (!keyer.IsIdle()) {
 *   keyer.Tick(esp_timer_get_time());
 *   vTaskDelay(pdMS_TO_TICKS(10));
 * }
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

extern "C" {
#include "esp_err.h"
}

namespace hal {
class TxHal;
}

namespace audio_subsystem {
class AudioSubsystem;
}

namespace morse_decoder {
class MorseEncoder;
}

namespace text_keyer {

/**
 * @brief State of the text keyer
 */
enum class KeyerState : uint8_t {
  kIdle = 0,       // Not sending, ready for new text
  kSending = 1,    // Actively transmitting morse code
  kPaused = 2,     // Paused mid-transmission (can resume)
};

/**
 * @brief Element type being sent
 */
enum class ElementType : uint8_t {
  kDit = 0,
  kDah = 1,
  kIntraGap = 2,   // Gap between elements within a character
  kCharGap = 3,    // Gap between characters
  kWordGap = 4,    // Gap between words
};

/**
 * @brief Text-to-morse keyer with precise timing
 *
 * Thread-safe: All public methods use mutex protection.
 * ISR-safe: No, call Tick() from main loop only.
 */
class TextKeyer {
 public:
  TextKeyer();
  ~TextKeyer();

  /**
   * @brief Initialize the text keyer
   * @param tx_hal TX HAL for keying output (non-owning)
   * @param audio_subsystem Audio subsystem for sidetone (non-owning)
   * @return ESP_OK on success
   */
  esp_err_t Initialize(hal::TxHal* tx_hal, audio_subsystem::AudioSubsystem* audio_subsystem);

  /**
   * @brief Send text as morse code
   * @param text Text to send (A-Z, 0-9, punctuation, spaces)
   * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already sending
   *
   * Thread-safe: Yes
   */
  esp_err_t SendText(const std::string& text);

  /**
   * @brief Update keyer state machine (call from main loop)
   * @param now_us Current timestamp in microseconds
   *
   * Thread-safe: Yes
   * Must be called regularly (e.g., every 10ms) for precise timing
   */
  void Tick(int64_t now_us);

  /**
   * @brief Abort current transmission immediately
   *
   * Thread-safe: Yes
   */
  void Abort();

  /**
   * @brief Pause transmission (can be resumed)
   *
   * Thread-safe: Yes
   */
  void Pause();

  /**
   * @brief Resume paused transmission
   *
   * Thread-safe: Yes
   */
  void Resume();

  /**
   * @brief Set keying speed
   * @param wpm Words per minute (5-60 WPM typical range)
   *
   * Thread-safe: Yes
   * Can be changed mid-transmission (affects next element)
   */
  void SetSpeed(uint32_t wpm);

  /**
   * @brief Get current keying speed
   * @return WPM
   *
   * Thread-safe: Yes
   */
  uint32_t GetSpeed() const;

  /**
   * @brief Check if keyer is idle
   * @return true if idle, false if sending or paused
   *
   * Thread-safe: Yes
   */
  bool IsIdle() const;

  /**
   * @brief Check if keyer is sending
   * @return true if sending, false otherwise
   *
   * Thread-safe: Yes
   */
  bool IsSending() const;

  /**
   * @brief Get current keyer state
   * @return Current state
   *
   * Thread-safe: Yes
   */
  KeyerState GetState() const;

  /**
   * @brief Get current transmission progress
   * @param sent Number of characters sent (output)
   * @param total Total characters to send (output)
   *
   * Thread-safe: Yes
   */
  void GetProgress(size_t& sent, size_t& total) const;

 private:
  /**
   * @brief Internal state for sending sequence
   */
  struct SendState {
    std::string original_text;           // Original text being sent
    std::vector<std::string> patterns;   // Encoded morse patterns
    size_t current_pattern_index = 0;    // Index in patterns vector
    size_t current_element_index = 0;    // Index within current pattern
    ElementType current_element = ElementType::kDit;
    int64_t element_end_time_us = 0;     // When current element should finish
    bool key_active = false;             // Current key state (true = transmitting)
  };

  /**
   * @brief Calculate dit duration in microseconds
   * @param wpm Words per minute
   * @return Dit duration in microseconds
   */
  int64_t CalculateDitDurationUs(uint32_t wpm) const;

  /**
   * @brief Start sending next element in sequence
   * @param now_us Current timestamp in microseconds
   * @return true if element started, false if sequence complete
   */
  bool StartNextElement(int64_t now_us);

  /**
   * @brief Finish current element (unkey if needed)
   * @param now_us Current timestamp in microseconds
   */
  void FinishCurrentElement(int64_t now_us);

  /**
   * @brief Set key state (TX + sidetone)
   * @param active true = key TX, false = unkey
   */
  void SetKeyState(bool active);

  // Dependencies (non-owning pointers)
  hal::TxHal* tx_hal_ = nullptr;
  audio_subsystem::AudioSubsystem* audio_subsystem_ = nullptr;
  morse_decoder::MorseEncoder* encoder_ = nullptr;  // Owned

  // Configuration
  uint32_t speed_wpm_ = 20;

  // State machine
  KeyerState state_ = KeyerState::kIdle;
  SendState send_state_;

  // Thread safety
  mutable std::mutex mutex_;
};

}  // namespace text_keyer
