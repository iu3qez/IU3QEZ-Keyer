#pragma once

/**
 * @file diagnostics_subsystem.hpp
 * @brief Diagnostics Subsystem - Status LED and paddle activity visualization
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * Encapsulates visual feedback mechanisms (status LED, activity tracking),
 * separating diagnostics concerns from core keying/audio logic.
 *
 * RESPONSIBILITIES:
 * - Initialize and manage StatusLedController
 * - Track paddle activity state (dit/dah active, last transition time)
 * - Provide spinlock-protected state updates from ISR context
 * - Update LED animations based on paddle activity
 *
 * THREAD SAFETY:
 * - UpdatePaddleActivity() is ISR-safe (uses portENTER_CRITICAL_ISR)
 * - Tick() must be called from main loop context only
 *
 * USAGE PATTERN:
 * ```
 * DiagnosticsSubsystem diag;
 * diag.Initialize(device_config);
 *
 * // In ISR callback:
 * diag.UpdatePaddleActivity(line, active, timestamp_us);
 *
 * // In main loop:
 * diag.Tick();  // Update LED animation
 * ```
 */

#include "config/device_config.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "hal/paddle_hal.hpp"
#include "diagnostics_subsystem/led_driver.hpp"
#include "timeline/event_logger.hpp"

namespace diagnostics_subsystem {

/**
 * @brief Paddle activity state snapshot (thread-safe via spinlock).
 */
struct PaddleActivityState {
  bool dit_active;
  bool dah_active;
  int64_t last_transition_us;
};

/**
 * @brief Manages status LED and paddle activity visualization.
 */
class DiagnosticsSubsystem {
 public:
  DiagnosticsSubsystem();
  ~DiagnosticsSubsystem() = default;

  /**
   * @brief Initialize status LED controller.
   * @param device_config Device configuration (NeoPixel GPIO, LED count).
   * @return ESP_OK on success, error code if LED init fails (non-fatal).
   */
  esp_err_t Initialize(const config::DeviceConfig& device_config);

  /**
   * @brief Check if LED driver initialized successfully.
   */
  bool IsReady() const { return led_driver_.IsInitialized(); }

  /**
   * @brief Update paddle activity state (ISR-safe, uses spinlock).
   * @param line Paddle line (dit/dah/key).
   * @param active True if paddle pressed, false if released.
   * @param timestamp_us Event timestamp in microseconds.
   */
  void UpdatePaddleActivity(hal::PaddleLine line, bool active, int64_t timestamp_us);

  /**
   * @brief Update LED animations based on current paddle activity (call from main loop).
   */
  void Tick();

  /**
   * @brief Signal USB CDC initialization starting (visual feedback for user).
   *
   * Sets all LEDs to bright white for 1 second, then turns them off.
   * This gives the user time to open COM7/COM8 terminal before CDC ports activate.
   *
   * @note Call this AFTER Initialize() but BEFORE usb_early_init().
   */
  void SignalUsbInitStarting();

  /**
   * @brief Signal initialization checkpoint with LED color (debug aid).
   * @param r Red component (0-255)
   * @param g Green component (0-255)
   * @param b Blue component (0-255)
   * @param duration_ms Duration to show color (milliseconds)
   *
   * @note Temporarily overrides paddle activity visualization.
   */
  void SignalCheckpoint(uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms = 300);

  /**
   * @brief Signal WiFi connected (blue animation, 3 cycles)
   *
   * Plays progressive blue fill animation to indicate WiFi connection.
   * Call from WiFiSubsystem event handler when IP_EVENT_STA_GOT_IP.
   */
  void SignalWifiConnected();

  /**
   * @brief Signal boot phase with LED color (debug diagnostics)
   * @param phase Boot phase number (0=NVS, 1=Config, 2=Subsystems, 3=WiFi, 4=Done)
   */
  void SignalBootPhase(int phase);

  /**
   * @brief Signal WiFi connecting state (yellow pulse on center LED)
   */
  void SignalWifiConnecting();

  /**
   * @brief Signal WiFi error (red flash 3x)
   */
  void SignalWifiError();

  /**
   * @brief Start rainbow pattern animation (captive portal setup mode indicator).
   *
   * Cycles through full HSV color spectrum over 3 seconds at 50% brightness.
   * Call when captive portal activates in AP mode to indicate setup mode.
   *
   * @note Overrides normal paddle activity visualization until StopRainbowPattern() called.
   */
  void SetRainbowPattern();

  /**
   * @brief Stop rainbow pattern and return to normal LED status indication.
   */
  void StopRainbowPattern();

  /**
   * @brief Get timeline logger reference for diagnostics export.
   */
  timeline::EventLogger<1024>* GetTimelineLogger() { return timeline_logger_; }

  /**
   * @brief Set timeline logger reference (injected from KeyingSubsystem).
   */
  void SetTimelineLogger(timeline::EventLogger<1024>* logger) { timeline_logger_ = logger; }

  /**
   * @brief Apply runtime configuration changes (hot-reload).
   *
   * Currently a stub - no runtime-changeable diagnostic parameters yet.
   * Future: LED brightness, word gap timeout, animation settings.
   *
   * @param device_config Updated device configuration
   */
  void ApplyConfig(const config::DeviceConfig& device_config);

 private:
  /**
   * @brief LED color (RGB)
   */
  struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
  };

  /**
   * @brief Apply base frame (paddle activity + word gap indicator)
   */
  void ApplyBaseFrame(int64_t now_us);

  /**
   * @brief Update WiFi animation
   */
  void UpdateWifiAnimation(int64_t now_us);

  /**
   * @brief Update rainbow pattern animation (captive portal mode)
   */
  void UpdateRainbowAnimation(int64_t now_us);

  /**
   * @brief Convert HSV color space to RGB
   * @param hue Hue angle in degrees (0-360)
   * @param saturation Saturation (0-100)
   * @param value Brightness value (0-100)
   * @param r Output red component (0-255)
   * @param g Output green component (0-255)
   * @param b Output blue component (0-255)
   */
  static void HsvToRgb(float hue, float saturation, float value, uint8_t& r, uint8_t& g, uint8_t& b);

  /**
   * @brief Render frame buffer to LED hardware
   */
  void Render();

  /**
   * @brief Set pixel in frame buffer
   */
  void SetPixel(size_t index, uint8_t r, uint8_t g, uint8_t b);

  /**
   * @brief Clear frame buffer
   */
  void ClearFrameBuffer();

  // LED hardware driver (HAL)
  LedDriver led_driver_;

  // Frame buffer
  Color frame_[7];  // Max 7 LEDs (configurable in device_config)
  size_t led_count_ = 0;

  // Paddle activity state (spinlock-protected)
  PaddleActivityState paddle_activity_state_;
  portMUX_TYPE paddle_state_spinlock_;

  // Configuration
  uint32_t word_gap_timeout_us_ = 0;
  uint32_t animation_step_us_ = 0;

  // Animation state
  bool wifi_animation_active_ = false;
  size_t animation_step_ = 0;
  int64_t animation_last_step_us_ = 0;

  // Rainbow pattern state (captive portal setup mode)
  bool rainbow_pattern_active_ = false;
  float rainbow_hue_ = 0.0f;  // Current hue angle (0-360 degrees)
  int64_t rainbow_last_update_us_ = 0;

  // Rendering throttle
  int64_t last_render_us_ = 0;

  // Timeline logger (borrowed reference from KeyingSubsystem)
  timeline::EventLogger<1024>* timeline_logger_;
};

}  // namespace diagnostics_subsystem
