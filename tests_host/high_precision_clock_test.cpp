#include "hal/high_precision_clock.hpp"

#include "gtest/gtest.h"

#include "esp_err.h"
#include "support/fake_esp_idf.hpp"

namespace {

class HighPrecisionClockTest : public ::testing::Test {
 protected:
  void SetUp() override { fake_esp_idf_reset(); }
};

}  // namespace

TEST_F(HighPrecisionClockTest, InitializeReturnsOk) {
  hal::HighPrecisionClock clock;
  EXPECT_EQ(ESP_OK, clock.Initialize());
}

TEST_F(HighPrecisionClockTest, NowMicrosReflectsTimer) {
  hal::HighPrecisionClock clock;
  ASSERT_EQ(ESP_OK, clock.Initialize());

  fake_esp_timer_set_time(1234567);
  EXPECT_EQ(1234567, clock.NowMicros());

  fake_esp_timer_set_time(987654321);
  EXPECT_EQ(987654321, clock.NowMicros());
}

TEST_F(HighPrecisionClockTest, NowMillisConvertsMicrosToMillis) {
  hal::HighPrecisionClock clock;
  ASSERT_EQ(ESP_OK, clock.Initialize());

  fake_esp_timer_set_time(1'234'567);
  EXPECT_EQ(1234, clock.NowMillis());

  fake_esp_timer_set_time(999);
  EXPECT_EQ(0, clock.NowMillis());
}
