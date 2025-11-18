/**
 * @file adaptive_timing_classifier.cpp
 * @brief Implementation of adaptive morse timing classifier
 */

#include "morse_decoder/adaptive_timing_classifier.hpp"

#include <algorithm>
#include <cinttypes>  // For PRIu32, PRId64
#include "esp_log.h"

namespace {
constexpr char kLogTag[] = "morse_decoder";
}  // namespace

namespace morse_decoder {

AdaptiveTimingClassifier::AdaptiveTimingClassifier(float tolerance_percent)
    : dit_avg_us_(kDefaultDitUs),
      dah_avg_us_(kDefaultDahUs),
      tolerance_percent_(tolerance_percent),
      dit_sample_count_(0),
      dah_sample_count_(0) {
  ESP_LOGI(kLogTag, "Classifier initialized: tolerance=%.1f%%, default_wpm=20",
           tolerance_percent_);
}

KeyEvent AdaptiveTimingClassifier::ClassifyDuration(int64_t duration_us, bool was_key_on) {
  if (duration_us <= 0) {
    ESP_LOGW(kLogTag, "Invalid duration: %lld μs", duration_us);
    return KeyEvent::kUnknown;
  }

  if (was_key_on) {
    // Key-on duration: classify as dit or dah
    const int64_t dit_threshold = ComputeDitThreshold();

    if (duration_us < dit_threshold) {
      // Classify as DIT
      UpdateDitAverage(duration_us);
      ESP_LOGD(kLogTag, "Key-on: %lld μs → DIT (threshold: %lld μs)", duration_us, dit_threshold);
      return KeyEvent::kDit;
    } else {
      // Classify as DAH
      UpdateDahAverage(duration_us);
      ESP_LOGD(kLogTag, "Key-on: %lld μs → DAH (threshold: %lld μs)", duration_us, dit_threshold);
      return KeyEvent::kDah;
    }
  } else {
    // Key-off duration: classify gap type
    // Check if we have enough samples for reliable gap classification
    if (dit_sample_count_ + dah_sample_count_ < kMinSamplesForClassification) {
      ESP_LOGD(kLogTag, "Key-off: %lld μs → UNKNOWN (warm-up)", duration_us);
      return KeyEvent::kUnknown;
    }

    // Calculate gap thresholds based on dit average
    // Intra-character gap: ~1 dit unit (threshold at 2 dit units)
    const int64_t char_gap_threshold = dit_avg_us_ * 2;

    // Inter-character gap: ~3 dit units (threshold at 5 dit units for word gap)
    const int64_t word_gap_threshold = dit_avg_us_ * 5;

    if (duration_us < char_gap_threshold) {
      ESP_LOGD(kLogTag, "Key-off: %lld μs → INTRA gap", duration_us);
      return KeyEvent::kIntraGap;
    } else if (duration_us < word_gap_threshold) {
      ESP_LOGD(kLogTag, "Key-off: %lld μs → CHAR gap", duration_us);
      return KeyEvent::kCharGap;
    } else {
      ESP_LOGD(kLogTag, "Key-off: %lld μs → WORD gap", duration_us);
      return KeyEvent::kWordGap;
    }
  }
}

uint32_t AdaptiveTimingClassifier::GetWPM() const {
  if (dit_avg_us_ <= 0) {
    return 0;
  }

  // WPM = 1,200,000 / dit_avg_us (PARIS standard)
  int64_t wpm = 1'200'000 / dit_avg_us_;

  // Clamp to reasonable range (10-60 WPM)
  if (wpm < 10) return 10;
  if (wpm > 60) return 60;

  return static_cast<uint32_t>(wpm);
}

TimingStats AdaptiveTimingClassifier::GetTimingStats() const {
  TimingStats stats;
  stats.avg_dit_us = dit_avg_us_;
  stats.avg_dah_us = dah_avg_us_;
  stats.ratio = (dit_avg_us_ > 0) ? static_cast<float>(dah_avg_us_) / static_cast<float>(dit_avg_us_) : 0.0f;
  stats.dit_sample_count = dit_sample_count_;
  stats.dah_sample_count = dah_sample_count_;
  return stats;
}

void AdaptiveTimingClassifier::Reset() {
  dit_avg_us_ = kDefaultDitUs;
  dah_avg_us_ = kDefaultDahUs;
  dit_sample_count_ = 0;
  dah_sample_count_ = 0;
  ESP_LOGI(kLogTag, "Classifier reset to defaults");
}

// --- Private Methods ---

int64_t AdaptiveTimingClassifier::ComputeDitThreshold() const {
  // Threshold is 1.5x the dit average, adjusted by tolerance
  // This places the threshold between typical dit and dah durations
  const float tolerance_factor = 1.0f + (tolerance_percent_ / 100.0f);
  return static_cast<int64_t>(dit_avg_us_ * 1.5f * tolerance_factor);
}

void AdaptiveTimingClassifier::UpdateDitAverage(int64_t measured_us) {
  if (dit_sample_count_ == 0) {
    // First sample: initialize directly
    dit_avg_us_ = measured_us;
  } else {
    // Apply Exponential Moving Average (EMA): α=0.3
    // new_avg = α * measured + (1-α) * old_avg
    dit_avg_us_ = static_cast<int64_t>(kEmaAlpha * measured_us + (1.0f - kEmaAlpha) * dit_avg_us_);
  }

  dit_sample_count_++;

  // Log WPM update periodically (every 10 samples to reduce verbosity)
  if (dit_sample_count_ % 10 == 0) {
    const float ratio = (dit_avg_us_ > 0) ? static_cast<float>(dah_avg_us_) / static_cast<float>(dit_avg_us_) : 0.0f;
    ESP_LOGD(kLogTag, "WPM updated: %" PRIu32 " (dit_avg: %" PRId64 " μs, dah_avg: %" PRId64 " μs, ratio: %.2f)",
             GetWPM(), dit_avg_us_, dah_avg_us_, ratio);
  }
}

void AdaptiveTimingClassifier::UpdateDahAverage(int64_t measured_us) {
  if (dah_sample_count_ == 0) {
    // First sample: initialize directly
    dah_avg_us_ = measured_us;
  } else {
    // Apply Exponential Moving Average (EMA): α=0.3
    dah_avg_us_ = static_cast<int64_t>(kEmaAlpha * measured_us + (1.0f - kEmaAlpha) * dah_avg_us_);
  }

  dah_sample_count_++;
}

}  // namespace morse_decoder
