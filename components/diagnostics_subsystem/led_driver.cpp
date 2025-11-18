#include "diagnostics_subsystem/led_driver.hpp"

extern "C" {
#include "esp_log.h"
}

#if HAS_LED_STRIP
extern "C" {
#include "led_strip.h"
}
#endif

namespace diagnostics_subsystem {

namespace {
constexpr char kLogTag[] = "LedDriver";
}

LedDriver::~LedDriver() {
#if HAS_LED_STRIP
  if (strip_ != nullptr) {
    led_strip_del(strip_);
    strip_ = nullptr;
  }
#else
  strip_ = nullptr;
#endif
  initialized_ = false;
  led_count_ = 0;
}

esp_err_t LedDriver::Initialize(gpio_num_t gpio, size_t led_count) {
  if (initialized_) {
    ESP_LOGW(kLogTag, "LED driver already initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (led_count == 0 || gpio == GPIO_NUM_NC) {
    ESP_LOGW(kLogTag, "Invalid LED configuration (gpio=%d, count=%zu)",
             static_cast<int>(gpio), led_count);
    return ESP_ERR_INVALID_ARG;
  }

  led_count_ = led_count;

#if HAS_LED_STRIP
  // Configure WS2812 LED strip
  led_strip_config_t strip_config{
      .strip_gpio_num = static_cast<int>(gpio),
      .max_leds = static_cast<uint32_t>(led_count),
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags = {
          .invert_out = false,
      },
  };

  // Configure RMT backend
  led_strip_rmt_config_t rmt_config = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10'000'000,  // 10 MHz resolution
      .mem_block_symbols = 0,
      .flags = {
          .with_dma = false,
      },
  };

  esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to create RMT LED strip: %s", esp_err_to_name(err));
    return err;
  }

  // Clear LEDs on init
  err = led_strip_clear(strip_);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to clear LED strip: %s", esp_err_to_name(err));
    led_strip_del(strip_);
    strip_ = nullptr;
    return err;
  }

  ESP_LOGI(kLogTag, "LED driver initialized (GPIO=%d, LEDs=%zu)",
           static_cast<int>(gpio), led_count);
#else
  ESP_LOGW(kLogTag, "LED strip driver not available; running in simulation mode");
#endif

  initialized_ = true;
  return ESP_OK;
}

esp_err_t LedDriver::SetPixel(size_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

  if (index >= led_count_) {
    return ESP_ERR_INVALID_ARG;
  }

#if HAS_LED_STRIP
  if (strip_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  const esp_err_t err = led_strip_set_pixel(strip_, static_cast<uint32_t>(index), r, g, b);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Failed to set pixel %zu: %s", index, esp_err_to_name(err));
    return err;
  }
#else
  (void)r;
  (void)g;
  (void)b;
#endif

  return ESP_OK;
}

esp_err_t LedDriver::Clear() {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

#if HAS_LED_STRIP
  if (strip_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  const esp_err_t err = led_strip_clear(strip_);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Failed to clear LED strip: %s", esp_err_to_name(err));
    return err;
  }
#endif

  return ESP_OK;
}

esp_err_t LedDriver::Refresh() {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

#if HAS_LED_STRIP
  if (strip_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  const esp_err_t err = led_strip_refresh(strip_);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "LED strip refresh failed: %s", esp_err_to_name(err));
    return err;
  }
#endif

  return ESP_OK;
}

}  // namespace diagnostics_subsystem
