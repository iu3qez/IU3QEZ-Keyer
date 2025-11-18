/**
 * @file timing_stats.hpp
 * @brief Statistics structure for morse code timing measurements
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * This structure holds the current state of the adaptive timing classifier's
 * internal averages. It provides visibility into the decoder's learned timing
 * parameters for debugging and display purposes.
 *
 * WPM Calculation:
 * - Standard PARIS timing: 50 dit units per word
 * - WPM = 60 seconds / (PARIS time) = 60 / (50 * dit_duration)
 * - WPM = 1200 / dit_duration_ms = 1200000 / dit_duration_us
 */

#pragma once

#include <cstdint>

namespace morse_decoder {

/**
 * @brief Timing statistics for adaptive morse decoder
 *
 * Captures the current learned dit/dah averages and their ratio.
 * Used for debugging, display, and performance monitoring.
 */
struct TimingStats {
  /** Average dit duration in microseconds */
  int64_t avg_dit_us = 0;

  /** Average dah duration in microseconds */
  int64_t avg_dah_us = 0;

  /** Dah-to-dit ratio (typically 3.0 for standard morse, range 2.5-4.0) */
  float ratio = 0.0f;

  /** Number of dit samples used to calculate average */
  uint32_t dit_sample_count = 0;

  /** Number of dah samples used to calculate average */
  uint32_t dah_sample_count = 0;

  /**
   * @brief Calculate detected Words Per Minute (WPM)
   * @return WPM based on average dit duration (clamped to 10-60 range)
   *
   * Uses standard PARIS timing: WPM = 1,200,000 / avg_dit_us
   * For example: 60ms dit → 20 WPM, 48ms dit → 25 WPM
   */
  uint32_t GetWPM() const {
    if (avg_dit_us <= 0) {
      return 0;  // Invalid or uninitialized
    }

    // Calculate WPM with bounds checking
    int64_t wpm = 1'200'000 / avg_dit_us;

    // Clamp to reasonable range (10-60 WPM)
    if (wpm < 10) return 10;
    if (wpm > 60) return 60;

    return static_cast<uint32_t>(wpm);
  }
};

}  // namespace morse_decoder
