#include "hal/tx_hal.hpp"

extern "C" {
#include "esp_log.h"
}

namespace hal {

namespace {
constexpr char kLogTag[] = "TxHal";
}

TxHal::~TxHal() {
  Shutdown();
}

esp_err_t TxHal::Initialize(gpio_num_t gpio, bool active_high) {
  if (initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

  if (gpio == GPIO_NUM_NC || gpio < GPIO_NUM_0) {
    ESP_LOGI(kLogTag, "TX output disabled (GPIO_NUM_NC)");
    gpio_ = GPIO_NUM_NC;
    initialized_ = true;  // Mark as initialized but disabled
    return ESP_OK;
  }

  gpio_ = gpio;
  active_high_ = active_high;

  // Configure GPIO as OUTPUT
  gpio_config_t io_conf{};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << static_cast<uint32_t>(gpio_));
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

  const esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to configure GPIO %d: %s", static_cast<int>(gpio_),
             esp_err_to_name(err));
    return err;
  }

  // Set initial state to inactive (unkeyed)
  const int inactive_level = active_high_ ? 0 : 1;
  gpio_set_level(gpio_, inactive_level);
  tx_active_ = false;

  initialized_ = true;
  ESP_LOGI(kLogTag, "TX output initialized (GPIO=%d, polarity=%s)",
           static_cast<int>(gpio_), active_high_ ? "active_high" : "active_low");
  return ESP_OK;
}

void TxHal::Shutdown() {
  if (!initialized_) {
    return;
  }

  // Ensure TX is inactive before shutdown
  if (gpio_ != GPIO_NUM_NC) {
    SetActive(false);
    gpio_reset_pin(gpio_);
  }

  gpio_ = GPIO_NUM_NC;
  initialized_ = false;
  tx_active_ = false;
}

void TxHal::SetActive(bool active) {
  if (!initialized_ || gpio_ == GPIO_NUM_NC) {
    return;
  }

  if (tx_active_ == active) {
    return;  // No change
  }

  const int level = (active == active_high_) ? 1 : 0;
  gpio_set_level(gpio_, level);
  tx_active_ = active;
}

}  // namespace hal
