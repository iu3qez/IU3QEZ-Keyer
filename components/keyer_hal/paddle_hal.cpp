#include "hal/paddle_hal.hpp"

#include <cstddef>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
}

namespace hal {

namespace {
constexpr char kLogTag[] = "PaddleHal";

inline size_t ToIndex(PaddleLine line) {
  return static_cast<size_t>(line);
}

gpio_config_t BuildGpioConfig(const PaddlePinConfig& pin_config) {
  gpio_config_t io_conf{};
  io_conf.intr_type = GPIO_INTR_ANYEDGE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (pin_config.gpio >= GPIO_NUM_0)
                             ? (1ULL << static_cast<uint32_t>(pin_config.gpio))
                             : 0ULL;
  io_conf.pull_down_en = pin_config.pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = pin_config.pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
  return io_conf;
}

esp_err_t EnsureIsrServiceInstalled() {
  static bool service_installed = false;
  if (service_installed) {
    return ESP_OK;
  }
  const esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (err == ESP_ERR_INVALID_STATE) {
    service_installed = true;
    return ESP_OK;
  }
  if (err == ESP_OK) {
    service_installed = true;
  }
  return err;
}

}  // namespace

PaddleHal::~PaddleHal() {
  Shutdown();
}

esp_err_t PaddleHal::Initialize(const PaddleHalConfig& config,
                                PaddleEventCallback callback,
                                void* context) {
  if (initialized_) {
    return ESP_ERR_INVALID_STATE;
  }
  callback_ = callback;
  callback_context_ = context;
  config_ = config;

#ifndef PADDLE_USE_POLLING
  // ISR mode: Install GPIO interrupt service
  const esp_err_t isr_err = EnsureIsrServiceInstalled();
  if (isr_err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to install GPIO ISR service: %s", esp_err_to_name(isr_err));
    return isr_err;
  }
#else
  ESP_LOGI(kLogTag, "Paddle HAL using POLLING mode (PADDLE_USE_POLLING defined)");
#endif

  const PaddlePinConfig pins[] = {config_.dit, config_.dah, config_.key};
  const PaddleLine lines[] = {PaddleLine::kDit, PaddleLine::kDah, PaddleLine::kKey};
  const size_t pin_count = sizeof(pins) / sizeof(pins[0]);

  for (size_t i = 0; i < pin_count; ++i) {
    const esp_err_t err = ConfigurePin(pins[i], lines[i]);
    if (err != ESP_OK) {
      Shutdown();
      return err;
    }
  }

  initialized_ = true;
  if (!HasConfiguredPins()) {
    ESP_LOGW(kLogTag, "No paddle GPIOs configured; hardware will remain idle until configured.");
  }
  return ESP_OK;
}

void PaddleHal::Shutdown() {
  if (!initialized_) {
    return;
  }

  const gpio_num_t pins[] = {config_.dit.gpio, config_.dah.gpio, config_.key.gpio};
  const size_t pin_count = sizeof(pins) / sizeof(pins[0]);
  for (size_t i = 0; i < pin_count; ++i) {
    if (!pin_configured_[i]) {
      continue;
    }
    gpio_isr_handler_remove(pins[i]);
    pin_configured_[i] = false;
  }

  initialized_ = false;
  callback_ = nullptr;
  callback_context_ = nullptr;
}

bool PaddleHal::HasConfiguredPins() const {
  return pin_configured_[0] || pin_configured_[1] || pin_configured_[2];
}

void IRAM_ATTR PaddleHal::HandleGpioInterrupt(void* arg) {
  if (arg == nullptr) {
    return;
  }
  auto* ctx = static_cast<GpioIsrContext*>(arg);
  if (ctx->self == nullptr) {
    return;
  }
  ctx->self->DispatchEdge(ctx->line);
}

void PaddleHal::DispatchEdge(PaddleLine line) const {
  if (callback_ == nullptr) {
    return;
  }
  const PaddlePinConfig& pin_config = PinConfigFor(line);
  if (pin_config.gpio == GPIO_NUM_NC) {
    return;
  }

  const int level = gpio_get_level(pin_config.gpio);
  const bool is_active = pin_config.active_low ? (level == 0) : (level != 0);

  PaddleEvent event{
      .line = line,
      .active = is_active,
      .timestamp_us = esp_timer_get_time(),
      .raw_level = static_cast<uint32_t>(level),
  };
  callback_(event, callback_context_);
}

esp_err_t PaddleHal::ConfigurePin(const PaddlePinConfig& pin_config, PaddleLine line) {
  if (pin_config.gpio == GPIO_NUM_NC) {
    return ESP_OK;
  }

  const gpio_config_t io_conf = BuildGpioConfig(pin_config);
  esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to configure GPIO %d: %s", static_cast<int>(pin_config.gpio),
             esp_err_to_name(err));
    return err;
  }

#ifndef PADDLE_USE_POLLING
  // ISR mode: Register interrupt handlers for edge detection
  err = gpio_set_intr_type(pin_config.gpio, GPIO_INTR_ANYEDGE);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to set interrupt type for GPIO %d: %s",
             static_cast<int>(pin_config.gpio), esp_err_to_name(err));
    return err;
  }

  const size_t index = ToIndex(line);
  gpio_contexts_[index] = GpioIsrContext{.self = this, .line = line};

  err = gpio_isr_handler_add(pin_config.gpio, &PaddleHal::HandleGpioInterrupt,
                             &gpio_contexts_[index]);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to install ISR handler for GPIO %d: %s",
             static_cast<int>(pin_config.gpio), esp_err_to_name(err));
    return err;
  }
#else
  // Polling mode: Initialize previous level for edge detection
  const int current_level = gpio_get_level(pin_config.gpio);
  switch (line) {
    case PaddleLine::kDit:
      last_dit_level_ = current_level;
      break;
    case PaddleLine::kDah:
      last_dah_level_ = current_level;
      break;
    case PaddleLine::kKey:
      last_key_level_ = current_level;
      break;
  }
#endif

  const size_t index = ToIndex(line);

  pin_configured_[index] = true;
  ESP_LOGI(kLogTag, "Configured GPIO %d for %s paddle input",
           static_cast<int>(pin_config.gpio),
           line == PaddleLine::kDit ? "DIT"
                                     : (line == PaddleLine::kDah ? "DAH" : "KEY"));
  return ESP_OK;
}

const PaddlePinConfig& PaddleHal::PinConfigFor(PaddleLine line) const {
  switch (line) {
    case PaddleLine::kDit:
      return config_.dit;
    case PaddleLine::kDah:
      return config_.dah;
    case PaddleLine::kKey:
      return config_.key;
  }
  return config_.dit;
}

#ifdef PADDLE_USE_POLLING
void PaddleHal::Poll() {
  if (!initialized_ || callback_ == nullptr) {
    return;
  }

  // Poll Dit paddle
  if (config_.dit.gpio != GPIO_NUM_NC) {
    const int current_level = gpio_get_level(config_.dit.gpio);
    if (current_level != last_dit_level_ && last_dit_level_ != -1) {
      const bool is_active = config_.dit.active_low ? (current_level == 0) : (current_level != 0);
      PaddleEvent event{
          .line = PaddleLine::kDit,
          .active = is_active,
          .timestamp_us = esp_timer_get_time(),
          .raw_level = static_cast<uint32_t>(current_level),
      };
      callback_(event, callback_context_);
    }
    last_dit_level_ = current_level;
  }

  // Poll Dah paddle
  if (config_.dah.gpio != GPIO_NUM_NC) {
    const int current_level = gpio_get_level(config_.dah.gpio);
    if (current_level != last_dah_level_ && last_dah_level_ != -1) {
      const bool is_active = config_.dah.active_low ? (current_level == 0) : (current_level != 0);
      PaddleEvent event{
          .line = PaddleLine::kDah,
          .active = is_active,
          .timestamp_us = esp_timer_get_time(),
          .raw_level = static_cast<uint32_t>(current_level),
      };
      callback_(event, callback_context_);
    }
    last_dah_level_ = current_level;
  }

  // Poll Key (straight key)
  if (config_.key.gpio != GPIO_NUM_NC) {
    const int current_level = gpio_get_level(config_.key.gpio);
    if (current_level != last_key_level_ && last_key_level_ != -1) {
      const bool is_active = config_.key.active_low ? (current_level == 0) : (current_level != 0);
      PaddleEvent event{
          .line = PaddleLine::kKey,
          .active = is_active,
          .timestamp_us = esp_timer_get_time(),
          .raw_level = static_cast<uint32_t>(current_level),
      };
      callback_(event, callback_context_);
    }
    last_key_level_ = current_level;
  }
}
#endif

}  // namespace hal
