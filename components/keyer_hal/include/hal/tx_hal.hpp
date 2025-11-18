#pragma once

/**
 * @file tx_hal.hpp
 * @brief Transmitter Output HAL - Controls TX key output GPIO
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * Encapsulates transmitter keying output (single GPIO control).
 * Separates TX hardware control from keying logic for testability.
 *
 * RESPONSIBILITIES:
 * - Initialize TX key output GPIO
 * - Control TX active/inactive state
 * - Handle active-high vs active-low polarity
 *
 * USAGE PATTERN:
 * ```
 * TxHal tx;
 * tx.Initialize(GPIO_NUM_5, true);  // GPIO 5, active high
 *
 * tx.SetActive(true);   // Key TX (set GPIO HIGH if active_high=true)
 * tx.SetActive(false);  // Unkey TX (set GPIO LOW if active_high=true)
 * ```
 */

extern "C" {
#include "driver/gpio.h"
#include "esp_err.h"
}

namespace hal {

/**
 * @brief Transmitter key output HAL
 */
class TxHal {
 public:
  TxHal() = default;
  ~TxHal();

  TxHal(const TxHal&) = delete;
  TxHal& operator=(const TxHal&) = delete;

  /**
   * @brief Initialize TX output GPIO.
   * @param gpio GPIO pin number (GPIO_NUM_NC = disabled)
   * @param active_high true = HIGH when transmitting, false = LOW when transmitting
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Initialize(gpio_num_t gpio, bool active_high);

  /**
   * @brief Shutdown TX HAL and release GPIO.
   */
  void Shutdown();

  /**
   * @brief Set TX active/inactive state.
   * @param active true = keying TX, false = unkeyed
   */
  void SetActive(bool active);

  /**
   * @brief Check if TX is currently active.
   */
  bool IsActive() const { return tx_active_; }

  /**
   * @brief Check if TX HAL is initialized.
   */
  bool IsInitialized() const { return initialized_; }

 private:
  gpio_num_t gpio_ = GPIO_NUM_NC;
  bool active_high_ = true;
  bool initialized_ = false;
  bool tx_active_ = false;
};

}  // namespace hal
