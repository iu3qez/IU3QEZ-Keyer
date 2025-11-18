#pragma once

/**
 * @file raii_handles.hpp
 * @brief RAII wrappers for ESP-IDF hardware resource handles
 *
 * PURPOSE:
 * Automatic resource cleanup via destructors eliminates manual cleanup code and prevents
 * resource leaks in error paths. Each wrapper uses the "Resource Acquisition Is Initialization"
 * idiom to guarantee deterministic cleanup when objects go out of scope.
 *
 * BENEFITS:
 * 1. Exception-safe (automatic cleanup even if exceptions enabled in future)
 * 2. Error path safety (no need to manually call cleanup in every failure branch)
 * 3. Move semantics (ownership transfer without double-free)
 * 4. Type safety (can't accidentally delete wrong handle type)
 *
 * USAGE PATTERN:
 * ```cpp
 * // Before RAII (manual cleanup):
 * i2c_master_bus_handle_t bus = nullptr;
 * esp_err_t err = i2c_new_master_bus(&cfg, &bus);
 * if (err != ESP_OK) return err;
 * // ... 50 lines of code ...
 * if (error_condition) {
 *   i2c_del_master_bus(bus);  // Easy to forget!
 *   return ESP_FAIL;
 * }
 * i2c_del_master_bus(bus);  // Duplicate cleanup code
 *
 * // After RAII (automatic cleanup):
 * I2cBusHandle bus;
 * esp_err_t err = bus.Create(&cfg);
 * if (err != ESP_OK) return err;  // bus automatically cleaned up
 * // ... 50 lines of code ...
 * if (error_condition) {
 *   return ESP_FAIL;  // bus automatically cleaned up
 * }
 * // bus automatically cleaned up on scope exit
 * ```
 */

#include <utility>  // std::exchange

extern "C" {
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "esp_io_expander_tca95xx_16bit.h"
}

namespace audio {

/**
 * @brief RAII wrapper for i2c_master_bus_handle_t
 *
 * Guarantees i2c_del_master_bus() is called on destruction if handle is valid.
 * Supports move semantics for ownership transfer.
 */
class I2cBusHandle {
 public:
  I2cBusHandle() = default;

  ~I2cBusHandle() { Reset(); }

  // Disable copy (handle ownership is unique)
  I2cBusHandle(const I2cBusHandle&) = delete;
  I2cBusHandle& operator=(const I2cBusHandle&) = delete;

  // Enable move (ownership transfer)
  I2cBusHandle(I2cBusHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

  I2cBusHandle& operator=(I2cBusHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  /**
   * @brief Create I2C master bus (wrapper for i2c_new_master_bus)
   * @param config I2C bus configuration
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Create(const i2c_master_bus_config_t* config) {
    Reset();  // Clean up existing handle if any
    return i2c_new_master_bus(config, &handle_);
  }

  /**
   * @brief Manually release the handle (called automatically by destructor)
   */
  void Reset() {
    if (handle_ != nullptr) {
      i2c_del_master_bus(handle_);
      handle_ = nullptr;
    }
  }

  /**
   * @brief Get raw handle for passing to ESP-IDF APIs
   */
  i2c_master_bus_handle_t Get() const { return handle_; }

  /**
   * @brief Check if handle is valid
   */
  bool IsValid() const { return handle_ != nullptr; }

  /**
   * @brief Implicit conversion to raw handle for API compatibility
   */
  operator i2c_master_bus_handle_t() const { return handle_; }

 private:
  i2c_master_bus_handle_t handle_ = nullptr;
};

/**
 * @brief RAII wrapper for i2s_chan_handle_t
 *
 * Guarantees i2s_channel_disable() + i2s_del_channel() are called on destruction.
 * Handles both disable and delete in correct order.
 */
class I2sChannelHandle {
 public:
  I2sChannelHandle() = default;

  ~I2sChannelHandle() { Reset(); }

  // Disable copy
  I2sChannelHandle(const I2sChannelHandle&) = delete;
  I2sChannelHandle& operator=(const I2sChannelHandle&) = delete;

  // Enable move
  I2sChannelHandle(I2sChannelHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)), enabled_(std::exchange(other.enabled_, false)) {}

  I2sChannelHandle& operator=(I2sChannelHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = std::exchange(other.handle_, nullptr);
      enabled_ = std::exchange(other.enabled_, false);
    }
    return *this;
  }

  /**
   * @brief Create I2S channel (wrapper for i2s_new_channel)
   * @param chan_cfg Channel configuration
   * @param tx_handle Pointer to receive TX handle (can be this handle)
   * @param rx_handle Pointer to receive RX handle (typically nullptr for TX-only)
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Create(const i2s_chan_config_t* chan_cfg, i2s_chan_handle_t* tx_handle,
                   i2s_chan_handle_t* rx_handle) {
    Reset();  // Clean up existing handle if any
    return i2s_new_channel(chan_cfg, tx_handle, rx_handle);
  }

  /**
   * @brief Initialize I2S channel in standard mode
   * @param std_cfg Standard mode configuration
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t InitStdMode(const i2s_std_config_t* std_cfg) {
    if (handle_ == nullptr) {
      return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_init_std_mode(handle_, std_cfg);
  }

  /**
   * @brief Enable I2S channel (wrapper for i2s_channel_enable)
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Enable() {
    if (handle_ == nullptr) {
      return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2s_channel_enable(handle_);
    if (err == ESP_OK) {
      enabled_ = true;
    }
    return err;
  }

  /**
   * @brief Disable I2S channel (wrapper for i2s_channel_disable)
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Disable() {
    if (handle_ != nullptr && enabled_) {
      esp_err_t err = i2s_channel_disable(handle_);
      enabled_ = false;
      return err;
    }
    return ESP_OK;
  }

  /**
   * @brief Manually release the handle (called automatically by destructor)
   */
  void Reset() {
    if (handle_ != nullptr) {
      if (enabled_) {
        i2s_channel_disable(handle_);
        enabled_ = false;
      }
      i2s_del_channel(handle_);
      handle_ = nullptr;
    }
  }

  /**
   * @brief Get raw handle for passing to ESP-IDF APIs
   */
  i2s_chan_handle_t Get() const { return handle_; }

  /**
   * @brief Check if handle is valid
   */
  bool IsValid() const { return handle_ != nullptr; }

  /**
   * @brief Check if channel is enabled
   */
  bool IsEnabled() const { return enabled_; }

  /**
   * @brief Implicit conversion to raw handle for API compatibility
   */
  operator i2s_chan_handle_t() const { return handle_; }

  /**
   * @brief Get address of handle for i2s_new_channel output parameter
   * @note Use this for Create() or direct i2s_new_channel() calls
   */
  i2s_chan_handle_t* GetAddressOf() { return &handle_; }

 private:
  i2s_chan_handle_t handle_ = nullptr;
  bool enabled_ = false;
};

/**
 * @brief RAII wrapper for esp_io_expander_handle_t
 *
 * Guarantees esp_io_expander_del() is called on destruction if handle is valid.
 */
class IoExpanderHandle {
 public:
  IoExpanderHandle() = default;

  ~IoExpanderHandle() { Reset(); }

  // Disable copy
  IoExpanderHandle(const IoExpanderHandle&) = delete;
  IoExpanderHandle& operator=(const IoExpanderHandle&) = delete;

  // Enable move
  IoExpanderHandle(IoExpanderHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

  IoExpanderHandle& operator=(IoExpanderHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  /**
   * @brief Create TCA95xx 16-bit I/O expander (wrapper for esp_io_expander_new_i2c_tca95xx_16bit)
   * @param i2c_bus I2C bus handle
   * @param address I2C device address
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t CreateTca95xx16Bit(i2c_master_bus_handle_t i2c_bus, uint32_t address) {
    Reset();  // Clean up existing handle if any
    return esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus, address, &handle_);
  }

  /**
   * @brief Manually release the handle (called automatically by destructor)
   */
  void Reset() {
    if (handle_ != nullptr) {
      esp_io_expander_del(handle_);
      handle_ = nullptr;
    }
  }

  /**
   * @brief Get raw handle for passing to ESP-IDF APIs
   */
  esp_io_expander_handle_t Get() const { return handle_; }

  /**
   * @brief Check if handle is valid
   */
  bool IsValid() const { return handle_ != nullptr; }

  /**
   * @brief Implicit conversion to raw handle for API compatibility
   */
  operator esp_io_expander_handle_t() const { return handle_; }

 private:
  esp_io_expander_handle_t handle_ = nullptr;
};

}  // namespace audio
