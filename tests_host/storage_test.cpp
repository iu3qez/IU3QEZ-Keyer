#include "config/device_config.hpp"

#include "gtest/gtest.h"

#include "esp_err.h"
#include "support/fake_esp_idf.hpp"

namespace {

class StorageTest : public ::testing::Test {
 protected:
  void SetUp() override { fake_esp_idf_reset(); }
};

}  // namespace

TEST_F(StorageTest, LoadReturnsDefaultsWhenNotInitialized) {
  config::Storage storage;
  const config::DeviceConfig defaults = storage.LoadOrDefault();

  EXPECT_EQ(3, defaults.paddle_pins.dit_gpio);
  EXPECT_TRUE(defaults.paddle_pins.paddles_active_low);
  EXPECT_EQ(7, defaults.neopixel.led_count);
}

TEST_F(StorageTest, InitializeFailsIfCalledTwice) {
  config::Storage storage;
  ASSERT_EQ(ESP_OK, storage.Initialize("keyer"));
  EXPECT_EQ(ESP_ERR_INVALID_STATE, storage.Initialize("keyer"));
}

TEST_F(StorageTest, SaveAndReloadPersistsConfiguration) {
  const char kNamespace[] = "keyer";
  {
    config::Storage storage;
    ASSERT_EQ(ESP_OK, storage.Initialize(kNamespace));

    config::DeviceConfig config{};
    config.paddle_pins.dit_gpio = 8;
    config.paddle_pins.dah_gpio = 9;
    config.paddle_pins.key_gpio = 10;
    config.paddle_pins.paddles_active_low = false;
    config.paddle_pins.use_pullups = false;
    config.paddle_pins.use_pulldowns = true;

    config.neopixel.gpio = 40;
    config.neopixel.led_count = 3;

    config.i2c.sda_gpio = 21;
    config.i2c.scl_gpio = 22;

    config.i2s.mclk_gpio = 1;
    config.i2s.bclk_gpio = 2;
    config.i2s.lrck_gpio = 3;
    config.i2s.dout_gpio = 4;

    config.codec.i2c_address = 0x33;
    config.io_expander.i2c_address = 0x44;
    config.io_expander.usb_selector_pin = 5;
    config.io_expander.pa_enable_pin = 6;

    ASSERT_EQ(ESP_OK, storage.Save(config));
  }

  config::Storage storage;
  ASSERT_EQ(ESP_OK, storage.Initialize(kNamespace));
  const config::DeviceConfig loaded = storage.LoadOrDefault();

  EXPECT_EQ(8, loaded.paddle_pins.dit_gpio);
  EXPECT_FALSE(loaded.paddle_pins.paddles_active_low);
  EXPECT_TRUE(loaded.paddle_pins.use_pulldowns);

  EXPECT_EQ(40, loaded.neopixel.gpio);
  EXPECT_EQ(3, loaded.neopixel.led_count);

  EXPECT_EQ(21, loaded.i2c.sda_gpio);
  EXPECT_EQ(22, loaded.i2c.scl_gpio);

  EXPECT_EQ(1, loaded.i2s.mclk_gpio);
  EXPECT_EQ(2, loaded.i2s.bclk_gpio);
  EXPECT_EQ(3, loaded.i2s.lrck_gpio);
  EXPECT_EQ(4, loaded.i2s.dout_gpio);

  EXPECT_EQ(0x33, loaded.codec.i2c_address);
  EXPECT_EQ(0x44, loaded.io_expander.i2c_address);
  EXPECT_EQ(5, loaded.io_expander.usb_selector_pin);
  EXPECT_EQ(6, loaded.io_expander.pa_enable_pin);
}
