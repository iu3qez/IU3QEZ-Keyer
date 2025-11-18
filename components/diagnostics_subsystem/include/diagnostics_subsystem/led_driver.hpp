#pragma once

/**
 * @file led_driver.hpp
 * @brief Hardware abstraction layer for WS2812 RGB LED strips
 *
 * Pure HAL for addressable LED hardware. No application logic.
 * Provides low-level primitives: set pixel, clear, refresh.
 *
 * ARCHITECTURE RATIONALE:
 * - Thin wrapper around ESP-IDF led_strip component
 * - No frame buffer (caller manages state)
 * - No animations or application logic
 * - Lives in diagnostics_subsystem (not reusable HAL)
 *
 * USAGE:
 * ```
 * LedDriver leds;
 * leds.Initialize(GPIO_NUM_38, 7);
 * leds.SetPixel(0, 255, 0, 0);  // Red
 * leds.Refresh();
 * ```
 */

#include <cstddef>
#include <cstdint>

extern "C" {
#include "driver/gpio.h"
#include "esp_err.h"
}

// Conditional led_strip.h include (same pattern as status_led.hpp)
#if defined(HAL_USE_LED_STRIP_STUB)
extern "C" {
#include "led_strip.h"
}
#define HAS_LED_STRIP 1
#elif defined(ESP_PLATFORM) && __has_include("led_strip.h")
extern "C" {
#include "led_strip.h"
}
#define HAS_LED_STRIP 1
#else
#define HAS_LED_STRIP 0
#endif

namespace diagnostics_subsystem {

#if HAS_LED_STRIP
using LedStripHandle = led_strip_handle_t;
#else
using LedStripHandle = void*;
#endif

/**
 * @brief Pure HAL for WS2812 RGB LED strips
 *
 * Manages hardware initialization and low-level pixel operations.
 * Does NOT manage frame buffers, animations, or application state.
 */
class LedDriver {
 public:
  LedDriver() = default;
  ~LedDriver();

  LedDriver(const LedDriver&) = delete;
  LedDriver& operator=(const LedDriver&) = delete;

  /**
   * @brief Initialize LED strip hardware
   *
   * @param gpio GPIO pin connected to LED data line
   * @param led_count Number of LEDs in strip
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Initialize(gpio_num_t gpio, size_t led_count);

  /**
   * @brief Set single pixel color (buffered, call Refresh to apply)
   *
   * @param index LED index (0-based)
   * @param r Red component (0-255)
   * @param g Green component (0-255)
   * @param b Blue component (0-255)
   * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index out of range
   */
  esp_err_t SetPixel(size_t index, uint8_t r, uint8_t g, uint8_t b);

  /**
   * @brief Clear all LEDs (buffered, call Refresh to apply)
   *
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Clear();

  /**
   * @brief Refresh LED strip (apply buffered changes to hardware)
   *
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Refresh();

  /**
   * @brief Check if driver initialized successfully
   */
  bool IsInitialized() const { return initialized_; }

  /**
   * @brief Get number of LEDs in strip
   */
  size_t GetLedCount() const { return led_count_; }

 private:
  LedStripHandle strip_ = nullptr;
  bool initialized_ = false;
  size_t led_count_ = 0;
};

}  // namespace diagnostics_subsystem
