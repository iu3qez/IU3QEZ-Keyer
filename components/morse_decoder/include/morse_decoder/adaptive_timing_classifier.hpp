/**
 * @file adaptive_timing_classifier.hpp
 * @brief Adaptive morse code timing classifier with WPM detection
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * This classifier uses Exponential Moving Average (EMA) to continuously adapt to
 * the operator's actual keying timing, supporting non-standard dit/dah ratios
 * (2.5:1 to 4:1 instead of strictly 3:1) and varying speeds (10-60 WPM).
 *
 * ALGORITHM DETAILS:
 * - EMA smoothing factor α = 0.3 balances noise reduction vs. adaptation speed
 * - Dynamic thresholds recalculated after each classified dit/dah
 * - Separate averages for dit and dah (no assumption of fixed ratio)
 * - Warm-up period: First 3 samples use default assumptions (20 WPM, 3:1 ratio)
 *
 * TIMING PRECISION:
 * - Input: Microsecond timestamps from esp_timer_get_time()
 * - Performance budget: ClassifyDuration() must complete in < 100 μs
 *
 * RESPONSIB ILITIES:
 * - Classify key-on durations as dit or dah
 * - Classify key-off durations as intra-character, inter-character, or inter-word gaps
 * - Maintain running averages of dit/dah durations (EMA)
 * - Detect operator's sending speed (WPM)
 * - Adapt to timing variations and non-standard ratios
 */

#pragma once

#include <cstdint>
#include "morse_decoder/timing_stats.hpp"

namespace morse_decoder {

/**
 * @brief Classification result for a keying event duration
 */
enum class KeyEvent : uint8_t {
  kDit = 0,      ///< Short key-on (dot)
  kDah = 1,      ///< Long key-on (dash)
  kIntraGap = 2, ///< Short key-off (within character, between dits/dahs)
  kCharGap = 3,  ///< Medium key-off (between characters)
  kWordGap = 4,  ///< Long key-off (between words)
  kUnknown = 255 ///< Cannot classify yet (warm-up period, invalid timing)
};

/**
 * @brief Adaptive timing classifier for morse code
 *
 * Learns the operator's timing characteristics and classifies durations into
 * dit/dah/gap categories. Adapts continuously to speed changes.
 *
 * Usage:
 * @code
 * AdaptiveTimingClassifier classifier(25.0f);  // 25% tolerance
 * int64_t key_on_duration = 58000;  // 58ms
 * KeyEvent event = classifier.ClassifyDuration(key_on_duration, true);
 * if (event == KeyEvent::kDit) {
 *   // Process dit...
 * }
 * uint32_t wpm = classifier.GetWPM();  // Get detected speed
 * @endcode
 */
class AdaptiveTimingClassifier {
 public:
  /**
   * @brief Construct classifier with specified tolerance
   * @param tolerance_percent Timing tolerance as percentage (±%, typically 10-50)
   *
   * Higher tolerance is more forgiving of inconsistent keying but may misclassify
   * borderline cases. Typical value: 25.0f (±25%).
   *
   * Initializes with default assumptions: 20 WPM (dit=60ms), 3:1 ratio (dah=180ms).
   */
  explicit AdaptiveTimingClassifier(float tolerance_percent = 25.0f);

  /**
   * @brief Classify a keying event duration
   * @param duration_us Duration in microseconds
   * @param was_key_on True if this was a key-on duration (dit/dah), false for key-off (gap)
   * @return Classified event type
   *
   * For key-on durations (was_key_on=true):
   * - Classifies as kDit or kDah based on threshold
   * - Updates internal dit/dah averages (EMA)
   *
   * For key-off durations (was_key_on=false):
   * - Classifies as kIntraGap, kCharGap, or kWordGap
   * - Does not update averages (gaps don't affect timing calibration)
   *
   * Returns kUnknown during warm-up period (< 3 samples).
   *
   * Performance: Guaranteed to complete in < 100 μs on ESP32-S3.
   */
  KeyEvent ClassifyDuration(int64_t duration_us, bool was_key_on);

  /**
   * @brief Get detected Words Per Minute (WPM)
   * @return Current WPM estimate (10-60 range)
   */
  uint32_t GetWPM() const;

  /**
   * @brief Get detailed timing statistics
   * @return Structure with dit/dah averages, ratio, sample counts
   */
  TimingStats GetTimingStats() const;

  /**
   * @brief Reset classifier to initial state
   *
   * Clears learned timing and returns to default assumptions (20 WPM, 3:1 ratio).
   * Use when operator changes or to clear bad calibration.
   */
  void Reset();

 private:
  /**
   * @brief Calculate dit/dah threshold for classification
   * @return Threshold in microseconds
   *
   * Threshold is midpoint between dit and dah averages, adjusted by tolerance:
   * threshold = dit_avg * 1.5 * (1.0 + tolerance%)
   */
  int64_t ComputeDitThreshold() const;

  /**
   * @brief Update dit average with new sample (EMA)
   * @param measured_us New dit duration measurement
   */
  void UpdateDitAverage(int64_t measured_us);

  /**
   * @brief Update dah average with new sample (EMA)
   * @param measured_us New dah duration measurement
   */
  void UpdateDahAverage(int64_t measured_us);

  // --- State Variables ---
  int64_t dit_avg_us_;        ///< Current dit average (microseconds)
  int64_t dah_avg_us_;        ///< Current dah average (microseconds)
  float tolerance_percent_;   ///< Timing tolerance (±%)
  uint32_t dit_sample_count_; ///< Number of dits seen
  uint32_t dah_sample_count_; ///< Number of dahs seen

  // --- Constants ---
  static constexpr float kEmaAlpha = 0.3f;           ///< EMA smoothing factor (α)
  static constexpr int64_t kDefaultDitUs = 60'000;   ///< Default dit @ 20 WPM
  static constexpr int64_t kDefaultDahUs = 180'000;  ///< Default dah @ 20 WPM (3:1 ratio)
  static constexpr uint32_t kMinSamplesForClassification = 3; ///< Warm-up period
};

}  // namespace morse_decoder
