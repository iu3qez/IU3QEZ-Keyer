/**
 * @file test_adaptive_timing_classifier.cpp
 * @brief Unit tests for adaptive timing classifier
 */

#include "morse_decoder/adaptive_timing_classifier.hpp"
#include "gtest/gtest.h"

using namespace morse_decoder;

namespace {

// Helper: Calculate dit duration for given WPM
// WPM = 1,200,000 / dit_us → dit_us = 1,200,000 / WPM
constexpr int64_t DitDurationUs(uint32_t wpm) {
  return 1'200'000 / wpm;
}

// Test fixture for classifier tests
class AdaptiveTimingClassifierTest : public ::testing::Test {
 protected:
  AdaptiveTimingClassifier classifier_{25.0f};  // 25% tolerance
};

// Test 1: Default initialization
TEST_F(AdaptiveTimingClassifierTest, DefaultInitialization) {
  // Verify default WPM is 20
  EXPECT_EQ(20u, classifier_.GetWPM());

  // Verify default timing stats
  auto stats = classifier_.GetTimingStats();
  EXPECT_EQ(60'000, stats.avg_dit_us);   // 20 WPM → 60ms dit
  EXPECT_EQ(180'000, stats.avg_dah_us);  // 3:1 ratio → 180ms dah
  EXPECT_FLOAT_EQ(3.0f, stats.ratio);
  EXPECT_EQ(0u, stats.dit_sample_count);
  EXPECT_EQ(0u, stats.dah_sample_count);
}

// Test 2: Classify dit and dah at 20 WPM
TEST_F(AdaptiveTimingClassifierTest, ClassifyDitDahAt20Wpm) {
  // Feed dit (60ms at 20 WPM)
  auto event1 = classifier_.ClassifyDuration(60'000, true);
  EXPECT_EQ(KeyEvent::kDit, event1);

  // Feed dah (180ms at 20 WPM)
  auto event2 = classifier_.ClassifyDuration(180'000, true);
  EXPECT_EQ(KeyEvent::kDah, event2);

  // Feed another dit
  auto event3 = classifier_.ClassifyDuration(60'000, true);
  EXPECT_EQ(KeyEvent::kDit, event3);

  // Verify WPM remains approximately 20
  EXPECT_GE(classifier_.GetWPM(), 18u);
  EXPECT_LE(classifier_.GetWPM(), 22u);

  // Verify sample counts
  auto stats = classifier_.GetTimingStats();
  EXPECT_EQ(2u, stats.dit_sample_count);  // 2 dits
  EXPECT_EQ(1u, stats.dah_sample_count);  // 1 dah
}

// Test 3: Classify gaps (intra-character, inter-character, inter-word)
TEST_F(AdaptiveTimingClassifierTest, ClassifyGaps) {
  // Need at least 3 samples for gap classification
  classifier_.ClassifyDuration(60'000, true);  // dit
  classifier_.ClassifyDuration(180'000, true); // dah
  classifier_.ClassifyDuration(60'000, true);  // dit

  // Now classify gaps (key-off durations)
  // Intra-character gap: ~1 dit unit (60ms)
  auto gap1 = classifier_.ClassifyDuration(60'000, false);
  EXPECT_EQ(KeyEvent::kIntraGap, gap1);

  // Inter-character gap: ~3 dit units (180ms)
  auto gap2 = classifier_.ClassifyDuration(180'000, false);
  EXPECT_EQ(KeyEvent::kCharGap, gap2);

  // Inter-word gap: ~7 dit units (420ms)
  auto gap3 = classifier_.ClassifyDuration(420'000, false);
  EXPECT_EQ(KeyEvent::kWordGap, gap3);
}

// Test 4: Adaptive WPM detection (speed change from 20 to 25 WPM)
TEST_F(AdaptiveTimingClassifierTest, AdaptiveWpmDetection) {
  // Start at 20 WPM (dit=60ms)
  EXPECT_EQ(20u, classifier_.GetWPM());

  // Feed 10 dits at 48ms each (25 WPM)
  const int64_t dit_25wpm = DitDurationUs(25);  // 48ms
  for (int i = 0; i < 10; i++) {
    classifier_.ClassifyDuration(dit_25wpm, true);
  }

  // Verify WPM converges to 25 (±2 WPM tolerance)
  uint32_t wpm = classifier_.GetWPM();
  EXPECT_GE(wpm, 23u);
  EXPECT_LE(wpm, 27u);

  // Verify dit average updated
  auto stats = classifier_.GetTimingStats();
  EXPECT_LT(stats.avg_dit_us, 55'000);  // Should be closer to 48ms than initial 60ms
}

// Test 5: Non-standard ratio (4:1 instead of 3:1)
TEST_F(AdaptiveTimingClassifierTest, NonStandardRatio4To1) {
  // Feed alternating dit (80ms) and dah (320ms) - 4:1 ratio
  // This corresponds to ~15 WPM with 4:1 ratio

  // Feed 5 cycles of dit-dah
  for (int i = 0; i < 5; i++) {
    auto dit_event = classifier_.ClassifyDuration(80'000, true);  // 80ms dit
    EXPECT_EQ(KeyEvent::kDit, dit_event);

    auto dah_event = classifier_.ClassifyDuration(320'000, true); // 320ms dah
    EXPECT_EQ(KeyEvent::kDah, dah_event);
  }

  // Verify both classified correctly (sample counts should be equal)
  auto stats = classifier_.GetTimingStats();
  EXPECT_EQ(5u, stats.dit_sample_count);
  EXPECT_EQ(5u, stats.dah_sample_count);

  // Verify ratio converges to ~4.0
  EXPECT_GE(stats.ratio, 3.5f);
  EXPECT_LE(stats.ratio, 4.5f);

  // Verify WPM calculated from dit (1,200,000 / 80,000 = 15 WPM)
  EXPECT_GE(classifier_.GetWPM(), 13u);
  EXPECT_LE(classifier_.GetWPM(), 17u);
}

// Test 6: Reset clears state
TEST_F(AdaptiveTimingClassifierTest, ResetClearsState) {
  // Feed some samples
  classifier_.ClassifyDuration(60'000, true);  // dit
  classifier_.ClassifyDuration(180'000, true); // dah
  classifier_.ClassifyDuration(60'000, true);  // dit

  // Verify state changed
  auto stats_before = classifier_.GetTimingStats();
  EXPECT_GT(stats_before.dit_sample_count, 0u);
  EXPECT_GT(stats_before.dah_sample_count, 0u);

  // Reset
  classifier_.Reset();

  // Verify state returned to defaults
  auto stats_after = classifier_.GetTimingStats();
  EXPECT_EQ(60'000, stats_after.avg_dit_us);
  EXPECT_EQ(180'000, stats_after.avg_dah_us);
  EXPECT_EQ(0u, stats_after.dit_sample_count);
  EXPECT_EQ(0u, stats_after.dah_sample_count);
  EXPECT_EQ(20u, classifier_.GetWPM());
}

// Test 7: Invalid duration handling
TEST_F(AdaptiveTimingClassifierTest, InvalidDuration) {
  // Negative duration
  auto event1 = classifier_.ClassifyDuration(-1000, true);
  EXPECT_EQ(KeyEvent::kUnknown, event1);

  // Zero duration
  auto event2 = classifier_.ClassifyDuration(0, true);
  EXPECT_EQ(KeyEvent::kUnknown, event2);
}

// Test 8: Warm-up period for gaps
TEST_F(AdaptiveTimingClassifierTest, GapWarmUpPeriod) {
  // Without enough key-on samples, gap classification should return kUnknown
  auto gap = classifier_.ClassifyDuration(100'000, false);
  EXPECT_EQ(KeyEvent::kUnknown, gap);

  // After 3 key-on samples, gap classification should work
  classifier_.ClassifyDuration(60'000, true);  // dit (1)
  classifier_.ClassifyDuration(60'000, true);  // dit (2)
  classifier_.ClassifyDuration(60'000, true);  // dit (3)

  auto gap2 = classifier_.ClassifyDuration(100'000, false);
  EXPECT_NE(KeyEvent::kUnknown, gap2);  // Should classify as some gap type
}

}  // namespace
