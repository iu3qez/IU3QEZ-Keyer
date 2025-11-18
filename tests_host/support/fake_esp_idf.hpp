#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "driver/gpio.h"
#include "esp_err.h"
#include "led_strip.h"

struct FakeGpioStateSnapshot {
  bool configured = false;
  gpio_config_t config{};
  gpio_int_type_t intr_type = GPIO_INTR_DISABLE;
  int level = 0;
  bool isr_installed = false;
};

struct FakeNvsSnapshotEntry {
  std::string key;
  int64_t value = 0;
  bool is_u8 = false;
  bool is_string = false;
  std::string string_value;
};

struct FakeLedStripSnapshot {
  size_t led_count = 0;
  std::vector<std::array<uint8_t, 3>> pixels;
  int refresh_count = 0;
};

void fake_esp_idf_reset();

void fake_esp_reset_time();
void fake_esp_timer_set_time(int64_t time_us);
void fake_esp_timer_advance(int64_t delta_us);

void fake_gpio_reset();
void fake_gpio_set_level(gpio_num_t gpio, int level);
FakeGpioStateSnapshot fake_gpio_snapshot(gpio_num_t gpio);
void fake_gpio_trigger(gpio_num_t gpio);
void fake_gpio_set_install_result(esp_err_t result);

void fake_nvs_reset();
std::vector<FakeNvsSnapshotEntry> fake_nvs_snapshot(const std::string& ns_name);

void fake_led_strip_reset();
FakeLedStripSnapshot fake_led_strip_snapshot(led_strip_handle_t handle);
std::vector<led_strip_handle_t> fake_led_strip_handles();
