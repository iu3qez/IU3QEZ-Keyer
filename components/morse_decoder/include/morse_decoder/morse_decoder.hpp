/**
 * @file morse_decoder.hpp
 * @brief Main morse decoder facade
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * The MorseDecoder is the top-level facade that orchestrates timing classification
 * and pattern-to-character conversion. It maintains state machine logic for
 * accumulating dit/dah patterns into complete characters.
 *
 * RESPONSIBILITIES:
 * - Accept classified KeyEvents from AdaptiveTimingClassifier
 * - Accumulate dit/dah patterns into character buffer
 * - Detect character boundaries (kCharGap) and word boundaries (kWordGap)
 * - Lookup patterns in MorseTable and emit decoded characters
 * - Maintain circular buffer of decoded text (100 characters default)
 *
 * STATE MACHINE:
 * - IDLE: Waiting for first dit/dah
 * - RECEIVING: Accumulating dits/dahs into pattern
 * - CHAR_GAP: Character boundary detected, finalize pattern
 * - WORD_GAP: Word boundary detected, finalize + add space
 *
 * THREAD SAFETY:
 * Thread-safe for concurrent access from ISR context (ProcessEvent) and
 * console/Web UI context (GetDecodedText). Protected by std::mutex.
 *
 * PERFORMANCE:
 * - ProcessEvent() target: < 200 μs on ESP32-S3
 * - Pattern buffer: max 6 elements (ITU longest code)
 * - Decoded buffer: circular, max 100 characters
 */

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "morse_decoder/adaptive_timing_classifier.hpp"
#include "morse_decoder/morse_table.hpp"
#include "timeline/event_logger.hpp"

namespace morse_decoder {

/**
 * @brief Decoder state machine states
 */
enum class DecoderState : uint8_t {
  kIdle = 0,       ///< Waiting for first dit/dah
  kReceiving = 1,  ///< Accumulating pattern (dit/dah sequence)
  kCharGap = 2,    ///< Character gap detected, finalize pattern
  kWordGap = 3     ///< Word gap detected, finalize + add space
};

/**
 * @brief Configuration options for MorseDecoder
 */
struct MorseDecoderConfig {
  /// Maximum decoded buffer size (characters)
  /// Older characters are discarded when buffer is full
  size_t buffer_size = 100;

  /// Enable verbose logging for debugging
  /// WARNING: High-frequency logging can affect performance
  bool enable_logging = false;

  /// Pointer to timing classifier (optional, for stats access)
  /// If provided, GetDetectedWPM() will query classifier directly
  AdaptiveTimingClassifier* timing_classifier = nullptr;
};

/**
 * @class MorseDecoder
 * @brief Main morse decoder facade with state machine and pattern accumulation
 *
 * Usage:
 * @code
 * MorseDecoderConfig config;
 * config.buffer_size = 100;
 * config.timing_classifier = &classifier;
 * MorseDecoder decoder(config);
 *
 * // Feed classified events from timing classifier
 * decoder.ProcessEvent(KeyEvent::kDit);
 * decoder.ProcessEvent(KeyEvent::kDit);
 * decoder.ProcessEvent(KeyEvent::kIntraGap);
 * decoder.ProcessEvent(KeyEvent::kDit);
 * decoder.ProcessEvent(KeyEvent::kCharGap);  // Finalize "S" (...)
 *
 * // Get decoded text
 * std::string text = decoder.GetDecodedText();  // "S"
 *
 * // Get last decoded character
 * char last = decoder.GetLastChar();  // 'S'
 * @endcode
 */
class MorseDecoder {
 public:
  /**
   * @brief Construct decoder with default configuration
   *
   * Default: 100 character buffer, logging disabled, no timing classifier link.
   */
  MorseDecoder();

  /**
   * @brief Construct decoder with custom configuration
   * @param config Configuration options
   */
  explicit MorseDecoder(const MorseDecoderConfig& config);

  /**
   * @brief Process a classified keying event
   * @param event Classified event from AdaptiveTimingClassifier
   *
   * State machine transitions:
   * - IDLE + kDit/kDah → RECEIVING (start pattern accumulation)
   * - RECEIVING + kDit/kDah → RECEIVING (append to pattern)
   * - RECEIVING + kIntraGap → RECEIVING (ignore, within character)
   * - RECEIVING + kCharGap → CHAR_GAP (finalize character)
   * - RECEIVING + kWordGap → WORD_GAP (finalize + add space)
   * - CHAR_GAP/WORD_GAP + any → IDLE (reset state)
   *
   * Character finalization:
   * - Lookup pattern in MorseTable
   * - If found: append char to decoded buffer
   * - If not found: append '?' (unknown pattern)
   * - Reset pattern buffer
   *
   * Thread-safe: Yes (protected by mutex)
   * Performance: Typical < 150 μs, worst-case < 200 μs
   */
  void ProcessEvent(KeyEvent event);

  /**
   * @brief Get the entire decoded text buffer
   * @return Decoded text as string (up to buffer_size characters)
   *
   * Returns most recent decoded text. Older characters are discarded
   * when buffer exceeds configured size (circular buffer).
   *
   * Thread-safe: Yes
   */
  std::string GetDecodedText();

  /**
   * @brief Get the last decoded character
   * @return Last character, or '\0' if buffer is empty
   *
   * Thread-safe: Yes
   */
  char GetLastChar();

  /**
   * @brief Get the current pattern being accumulated
   * @return Pattern string (e.g., ".-" for partial 'A')
   *
   * Returns empty string if no pattern is being accumulated (IDLE state).
   *
   * Thread-safe: Yes
   * Use case: Debugging, console display of in-progress pattern
   */
  std::string GetCurrentPattern();

  /**
   * @brief Get detected WPM from timing classifier
   * @return WPM (10-60), or 0 if no classifier configured
   *
   * Requires config.timing_classifier to be set during construction.
   *
   * Thread-safe: Yes
   */
  uint32_t GetDetectedWPM() const;

  /**
   * @brief Reset decoder state
   *
   * Clears:
   * - Current pattern buffer
   * - Decoded text buffer
   * - State machine (back to IDLE)
   *
   * Does NOT reset timing classifier (call classifier.Reset() separately if needed).
   *
   * Thread-safe: Yes
   * Use case: User clicks "clear" button in Web UI, console "decoder reset" command
   */
  void Reset();

  /**
   * @brief Enable or disable the decoder
   * @param enabled True to enable, false to disable
   *
   * When disabled:
   * - ProcessEvent() calls are ignored (no-op)
   * - No state changes occur
   * - Existing decoded buffer is preserved
   *
   * Thread-safe: Yes
   */
  void SetEnabled(bool enabled);

  /**
   * @brief Check if decoder is enabled
   * @return True if enabled, false otherwise
   *
   * Thread-safe: Yes
   */
  bool IsEnabled() const;

  /**
   * @brief Get current decoder state (for debugging/UI)
   * @return Current state machine state
   *
   * Thread-safe: Yes
   */
  DecoderState GetState() const;

  /**
   * @brief Periodic tick to handle inactivity timeout
   * @param now_us Current timestamp in microseconds
   *
   * If the decoder has been in RECEIVING state for too long without new events,
   * forces finalization of the current pattern + word space. The timeout is
   * calculated dynamically based on detected WPM (7 dit units = word gap).
   *
   * Default timeout: 500ms (word gap @ 20 WPM)
   * Dynamic timeout: avg_dit_us * 7 (adapts to operator speed)
   *
   * This prevents decoded text from remaining "stuck" waiting for the next element.
   *
   * Call this periodically from main loop (e.g., every 50ms).
   *
   * Thread-safe: Yes
   */
  void Tick(int64_t now_us);

  /**
   * @brief Set timeline logger for gap marker emission (injected by ApplicationController)
   * @param logger Pointer to timeline EventLogger (non-owning, may be nullptr)
   *
   * When configured, decoder emits gap marker events (kGapMarker) to the timeline
   * whenever gaps are classified as inter-element, inter-character, or inter-word.
   * Used for real-time timeline visualization.
   *
   * Thread-safe: Yes
   */
  void SetTimelineLogger(timeline::EventLogger<1024>* logger);

 private:
  /**
   * @brief Finalize the current pattern and decode to character
   *
   * Called on kCharGap or kWordGap events.
   *
   * Steps:
   * 1. Lookup pattern in morse_table_
   * 2. If found: append character to decoded_buffer_
   * 3. If not found: append '?' (unknown)
   * 4. If kWordGap: also append ' ' (space)
   * 5. Maintain circular buffer (discard oldest if exceeds buffer_size)
   * 6. Reset current_pattern_
   * 7. Transition to IDLE
   *
   * Thread-safe: No (caller must hold mutex_)
   */
  void FinalizePattern(bool add_space);

  /**
   * @brief Append a character to the decoded buffer
   * @param c Character to append
   *
   * Maintains circular buffer behavior: if buffer exceeds buffer_size,
   * removes oldest character from front.
   *
   * Thread-safe: No (caller must hold mutex_)
   */
  void AppendToBuffer(char c);

  // Configuration
  MorseDecoderConfig config_;

  // Morse lookup table (immutable after construction)
  MorseTable morse_table_;

  // State machine
  DecoderState state_ = DecoderState::kIdle;

  // Current pattern being accumulated (e.g., ".-" for 'A')
  std::string current_pattern_;

  // Decoded text buffer (circular, max buffer_size characters)
  std::deque<char> decoded_buffer_;

  // Last decoded character (for quick access)
  char last_char_ = '\0';

  // Enabled flag
  bool enabled_ = true;

  // Activity tracking for inactivity timeout
  int64_t last_activity_time_us_ = 0;

  // Default inactivity timeout: word gap @ 20 WPM (7 dit units × 60ms = 420ms, rounded to 500ms)
  // Actual timeout is calculated dynamically based on detected WPM in Tick()
  static constexpr int64_t kInactivityTimeoutUs = 500'000;  // 500ms default

  // Timeline logger for gap marker emission (injected, non-owning)
  timeline::EventLogger<1024>* timeline_logger_ = nullptr;

  // Thread safety
  mutable std::mutex mutex_;
};

}  // namespace morse_decoder
