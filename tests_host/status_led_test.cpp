#include "stubs/led_strip.h"
#include "hal/status_led.hpp"

#include "gtest/gtest.h"

#include "esp_err.h"
#include "support/fake_esp_idf.hpp"

namespace {

class StatusLedControllerTest : public ::testing::Test {
 protected:
  void SetUp() override { fake_esp_idf_reset(); }
};

}  // namespace

TEST_F(StatusLedControllerTest, InitializeRequiresValidConfiguration) {
  hal::StatusLedController controller;
  hal::StatusLedConfig config{};

  EXPECT_EQ(ESP_ERR_INVALID_ARG, controller.Initialize(config));
  EXPECT_FALSE(controller.IsInitialized());

  config.data_gpio = 5;
  config.led_count = 0;
  EXPECT_EQ(ESP_ERR_INVALID_ARG, controller.Initialize(config));
  EXPECT_FALSE(controller.IsInitialized());
}

TEST_F(StatusLedControllerTest, TickReflectsWordGapState) {
  hal::StatusLedController controller;
  hal::StatusLedConfig config{};
  config.data_gpio = 6;
  config.led_count = 7;
  config.word_gap_timeout_ms = 100;
  config.animation_step_ms = 25;

  ASSERT_EQ(ESP_OK, controller.Initialize(config));
  auto handles = fake_led_strip_handles();
  ASSERT_EQ(1U, handles.size());
  const auto handle = handles.front();

  // No word gap completion yet -> last LED red.
  controller.SetPaddleActivity(false, false, 0);
  fake_esp_timer_set_time(50'000);
  controller.Tick();
  auto snapshot = fake_led_strip_snapshot(handle);
  ASSERT_EQ(config.led_count, snapshot.led_count);
  EXPECT_EQ(255, snapshot.pixels.back()[0]);  // Red channel
  EXPECT_EQ(0, snapshot.pixels.back()[1]);    // Green channel

  // Word gap exceeded -> last LED green.
  controller.SetPaddleActivity(false, false, 25'000);
  fake_esp_timer_set_time(200'000);
  controller.Tick();
  snapshot = fake_led_strip_snapshot(handle);
  EXPECT_EQ(0, snapshot.pixels.back()[0]);
  EXPECT_EQ(255, snapshot.pixels.back()[1]);
  EXPECT_EQ(32, snapshot.pixels.back()[2]);
  EXPECT_EQ(2, snapshot.refresh_count);
}

TEST_F(StatusLedControllerTest, WifiAnimationRunsForConfiguredCycles) {
  hal::StatusLedController controller;
  hal::StatusLedConfig config{};
  config.data_gpio = 7;
  config.led_count = 4;
  config.word_gap_timeout_ms = 200;
  config.animation_step_ms = 25;

  ASSERT_EQ(ESP_OK, controller.Initialize(config));
  auto handles = fake_led_strip_handles();
  ASSERT_EQ(1U, handles.size());
  const auto handle = handles.front();

  controller.StartWifiConnectedAnimation();

  fake_esp_timer_set_time(0);
  controller.Tick();  // Step 0

  auto snapshot = fake_led_strip_snapshot(handle);
  ASSERT_EQ(config.led_count, snapshot.led_count);
  EXPECT_EQ(0, snapshot.pixels[0][0]);
  EXPECT_EQ(96, snapshot.pixels[0][1]);
  EXPECT_EQ(255, snapshot.pixels[0][2]);

  const int total_steps = static_cast<int>(config.led_count * 3);  // kAnimationCycles
  for (int step = 1; step <= total_steps; ++step) {
    fake_esp_timer_advance(25'000);
    controller.Tick();
  }

  snapshot = fake_led_strip_snapshot(handle);
  // Animation completes and controller reverts to base frame (last LED red).
  EXPECT_EQ(255, snapshot.pixels.back()[0]);
  EXPECT_EQ(0, snapshot.pixels.back()[1]);
  EXPECT_EQ(32, snapshot.pixels.back()[2]);
}
