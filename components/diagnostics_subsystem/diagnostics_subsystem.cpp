/**
 * @file diagnostics_subsystem.cpp
 * @brief Diagnostics Subsystem implementation - Status LED visualization and paddle activity tracking
 *
 * IMPLEMENTATION OVERVIEW:
 * ========================
 * Manages WS2812 NeoPixel LED strip for visual feedback with three primary functions:
 * 1. **Paddle Activity Tracking**: ISR-safe state updates via spinlock for dit/dah events
 * 2. **LED Animations**: Color-coded status indication (WiFi, paddle activity, word gaps)
 * 3. **Timeline Integration**: Optional event logging for debugging keying behavior
 *
 * ARCHITECTURE NOTES:
 * ===================
 * - **Thread-Safe Design**: paddle_state_spinlock_ protects activity state updates from ISR context
 * - **Throttled Rendering**: kMinRenderIntervalUs (20ms) prevents LED flickering at 50Hz refresh rate
 * - **Color Palette**: Predefined constants for yellow (WiFi), green (dit), red (dah), blue (word gap)
 * - **Frame Buffer**: Double-buffered rendering via frame_[] array + led_driver_.SetPixel()
 *
 * LED ANIMATION STATES:
 * =====================
 * - **Yellow Pulse**: WiFi connecting (animated fade in/out over kAnimationCycles)
 * - **Green**: Dit paddle active (first LED)
 * - **Red**: Dah paddle active (second LED)
 * - **Blue Fade**: Word gap timeout expired (all LEDs, fades out over 500ms)
 * - **Off**: Idle state (no paddle activity, WiFi ready)
 *
 * TIMING CONSTANTS:
 * =================
 * - kMinRenderIntervalUs (20ms): Maximum LED update rate to prevent flicker
 * - kAnimationCycles (3): Number of WiFi animation pulse cycles before marking ready
 * - word_gap_timeout_us_: Configurable timeout for word gap detection (typically 7× dit length)
 *
 * CRITICAL SECTIONS:
 * ==================
 * UpdatePaddleActivity() uses portENTER_CRITICAL_ISR to safely update paddle_activity_state_
 * from ISR context. All other methods assume main loop context with interrupts enabled.
 *
 * DEPENDENCIES:
 * =============
 * - StatusLedController: WS2812 driver wrapper (components/diagnostics_subsystem/led_driver.hpp)
 * - PaddleHal: GPIO interrupt source for paddle events (components/keyer_hal/include/hal/paddle_hal.hpp)
 * - HighPrecisionClock: Microsecond timestamps for animation timing (components/keyer_hal/include/hal/high_precision_clock.hpp)
 * - EventLogger: Optional timeline recording (components/timeline/include/timeline/event_logger.hpp)
 */

#include "diagnostics_subsystem/diagnostics_subsystem.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "hal/high_precision_clock.hpp"

namespace diagnostics_subsystem {

namespace {
constexpr char kLogTag[] = "diagnostics_subsystem";

// LED colors
constexpr uint8_t kYellowR = 255;
constexpr uint8_t kYellowG = 180;
constexpr uint8_t kYellowB = 0;
constexpr uint8_t kGreenR = 0;
constexpr uint8_t kGreenG = 255;
constexpr uint8_t kGreenB = 32;
constexpr uint8_t kRedR = 255;
constexpr uint8_t kRedG = 0;
constexpr uint8_t kRedB = 32;
constexpr uint8_t kBlueR = 0;
constexpr uint8_t kBlueG = 96;
constexpr uint8_t kBlueB = 255;

// Timing constants
constexpr uint32_t kMinRenderIntervalUs = 20'000;  // 20 ms (50 Hz)
constexpr uint32_t kAnimationCycles = 3;  // WiFi animation cycles

// Rainbow pattern constants
constexpr uint8_t kRainbowBrightness = 128;  // 50% brightness (out of 255)
constexpr float kRainbowCycleDurationUs = 3'000'000.0f;  // 3 seconds for full 360° cycle

}  // namespace

DiagnosticsSubsystem::DiagnosticsSubsystem()
    : led_driver_(),
      frame_{},
      led_count_(0),
      paddle_activity_state_{false, false, 0},
      paddle_state_spinlock_(portMUX_INITIALIZER_UNLOCKED),
      word_gap_timeout_us_(0),
      animation_step_us_(0),
      wifi_animation_active_(false),
      animation_step_(0),
      animation_last_step_us_(0),
      last_render_us_(0),
      timeline_logger_(nullptr) {}

esp_err_t DiagnosticsSubsystem::Initialize(const config::DeviceConfig& device_config) {
  ESP_LOGI(kLogTag, "Initializing diagnostics subsystem");

  led_count_ = device_config.neopixel.led_count;
  if (led_count_ > 7) {
    ESP_LOGW(kLogTag, "LED count %zu exceeds frame buffer size 7, clamping",
             led_count_);
    led_count_ = 7;
  }

  // Initialize LED driver (HAL)
  const gpio_num_t gpio = static_cast<gpio_num_t>(device_config.neopixel.gpio);
  const esp_err_t err = led_driver_.Initialize(gpio, led_count_);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "LED driver init failed: %s (diagnostics disabled)",
             esp_err_to_name(err));
    return err;
  }

  // Configuration for animations
  constexpr uint32_t kWordGapTimeoutMs = 400;  // Word gap detection
  constexpr uint32_t kAnimationStepMs = 80;   // Animation frame rate
  word_gap_timeout_us_ = kWordGapTimeoutMs * 1000U;
  animation_step_us_ = kAnimationStepMs * 1000U;
  if (animation_step_us_ < kMinRenderIntervalUs) {
    animation_step_us_ = kMinRenderIntervalUs;
  }

  paddle_activity_state_.last_transition_us = hal::HighPrecisionClock::NowMicros();

  ESP_LOGI(kLogTag, "Diagnostics subsystem initialized (GPIO=%d, LEDs=%zu)",
           static_cast<int>(gpio), led_count_);
  return ESP_OK;
}

void DiagnosticsSubsystem::UpdatePaddleActivity(hal::PaddleLine line, bool active,
                                                int64_t timestamp_us) {
  portENTER_CRITICAL_ISR(&paddle_state_spinlock_);
  switch (line) {
    case hal::PaddleLine::kDit:
      paddle_activity_state_.dit_active = active;
      paddle_activity_state_.last_transition_us = timestamp_us;
      break;
    case hal::PaddleLine::kDah:
      paddle_activity_state_.dah_active = active;
      paddle_activity_state_.last_transition_us = timestamp_us;
      break;
    case hal::PaddleLine::kKey:
      paddle_activity_state_.last_transition_us = timestamp_us;
      break;
  }
  portEXIT_CRITICAL_ISR(&paddle_state_spinlock_);
}

void DiagnosticsSubsystem::SignalUsbInitStarting() {
  if (!led_driver_.IsInitialized()) {
    ESP_LOGW(kLogTag, "Cannot signal USB init - LED not initialized");
    return;
  }

  ESP_LOGI(kLogTag, "*** OPEN COM7/COM8 NOW - USB CDC initializing in 1 second ***");

  // Set all LEDs to bright white (R=255, G=255, B=255)
  for (size_t i = 0; i < led_count_; ++i) {
    led_driver_.SetPixel(i, 255, 255, 255);
  }
  led_driver_.Refresh();

  // Wait 1 second for user to open terminal
  vTaskDelay(pdMS_TO_TICKS(1000));

  // Clear LEDs (Tick() will resume normal paddle activity visualization)
  led_driver_.Clear();
  led_driver_.Refresh();

  ESP_LOGI(kLogTag, "USB init signal complete");
}

void DiagnosticsSubsystem::SignalCheckpoint(uint8_t r, uint8_t g, uint8_t b,
                                            uint32_t duration_ms) {
  if (!led_driver_.IsInitialized()) {
    return;  // Silent fail - not critical
  }

  for (size_t i = 0; i < led_count_; ++i) {
    led_driver_.SetPixel(i, r, g, b);
  }
  led_driver_.Refresh();

  if (duration_ms > 0) {
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
  }
}

void DiagnosticsSubsystem::SignalWifiConnected() {
  if (!led_driver_.IsInitialized()) {
    return;
  }
  wifi_animation_active_ = true;
  animation_step_ = 0;
  animation_last_step_us_ = 0;
  ESP_LOGI(kLogTag, "WiFi connected animation started");
}

void DiagnosticsSubsystem::SignalBootPhase(int phase) {
  if (!led_driver_.IsInitialized()) {
    return;
  }

  uint8_t r, g, b;
  switch (phase) {
    case 0: r = 255; g = 0;   b = 255; break;  // Magenta: NVS
    case 1: r = 0;   g = 255; b = 255; break;  // Cyan: Config
    case 2: r = 255; g = 128; b = 0;   break;  // Orange: Subsystems
    case 3: r = 255; g = 255; b = 0;   break;  // Yellow: WiFi init
    case 4: r = 0;   g = 255; b = 0;   break;  // Green: Boot complete
    default: r = 255; g = 0; b = 0; break;     // Red: Unknown
  }

  for (int i = 0; i < led_count_; i++) {
    led_driver_.SetPixel(i, r, g, b);
  }
  led_driver_.Refresh();
  vTaskDelay(pdMS_TO_TICKS(300));

  for (int i = 0; i < led_count_; i++) {
    led_driver_.SetPixel(i, 0, 0, 0);
  }
  led_driver_.Refresh();
}

void DiagnosticsSubsystem::SignalWifiConnecting() {
  if (!led_driver_.IsInitialized()) {
    return;
  }

  static uint8_t brightness = 0;
  brightness = (brightness + 20) % 256;

  if (led_count_ >= 7) {
    led_driver_.SetPixel(3, brightness, brightness, 0);
    led_driver_.Refresh();
  }
}

void DiagnosticsSubsystem::SignalWifiError() {
  if (!led_driver_.IsInitialized()) {
    return;
  }

  for (int flash = 0; flash < 3; flash++) {
    for (int i = 0; i < led_count_; i++) {
      led_driver_.SetPixel(i, 255, 0, 0);
    }
    led_driver_.Refresh();
    vTaskDelay(pdMS_TO_TICKS(150));

    for (int i = 0; i < led_count_; i++) {
      led_driver_.SetPixel(i, 0, 0, 0);
    }
    led_driver_.Refresh();
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

void DiagnosticsSubsystem::SetRainbowPattern() {
  if (!led_driver_.IsInitialized()) {
    return;
  }
  rainbow_pattern_active_ = true;
  rainbow_hue_ = 0.0f;
  rainbow_last_update_us_ = 0;
  ESP_LOGI(kLogTag, "Rainbow pattern started (captive portal setup mode)");
}

void DiagnosticsSubsystem::StopRainbowPattern() {
  if (!led_driver_.IsInitialized()) {
    return;
  }
  rainbow_pattern_active_ = false;
  ClearFrameBuffer();
  Render();
  ESP_LOGI(kLogTag, "Rainbow pattern stopped");
}

void DiagnosticsSubsystem::Tick() {
  if (!led_driver_.IsInitialized()) {
    return;
  }

  const int64_t now_us = esp_timer_get_time();

  // Throttle rendering to 50 Hz (20ms)
  if (last_render_us_ != 0 &&
      static_cast<uint32_t>(now_us - last_render_us_) < kMinRenderIntervalUs) {
    return;
  }

  // Snapshot paddle activity state (spinlock-protected)
  PaddleActivityState activity_snapshot;
  portENTER_CRITICAL(&paddle_state_spinlock_);
  activity_snapshot = paddle_activity_state_;
  portEXIT_CRITICAL(&paddle_state_spinlock_);

  // Apply frame based on mode (priority: rainbow > wifi animation > base frame)
  if (rainbow_pattern_active_) {
    UpdateRainbowAnimation(now_us);
  } else if (wifi_animation_active_) {
    UpdateWifiAnimation(now_us);
  } else {
    ApplyBaseFrame(now_us);
  }

  Render();
  last_render_us_ = now_us;
}

void DiagnosticsSubsystem::ApplyBaseFrame(int64_t now_us) {
  ClearFrameBuffer();

  // Get paddle state snapshot
  PaddleActivityState activity;
  portENTER_CRITICAL(&paddle_state_spinlock_);
  activity = paddle_activity_state_;
  portEXIT_CRITICAL(&paddle_state_spinlock_);

  // Paddle activity visualization:
  // - LED 0-1: DASH (dah paddle) - yellow when active
  // - LED 5-6: DIT (dit paddle) - yellow when active
  // - LED 3 (middle): Word gap indicator (green/red)

  if (led_count_ >= 7) {
    // Standard 7-LED configuration
    if (activity.dah_active) {
      SetPixel(0, kYellowR, kYellowG, kYellowB);  // DASH LED 1
      SetPixel(1, kYellowR, kYellowG, kYellowB);  // DASH LED 2
    }

    if (activity.dit_active) {
      SetPixel(5, kYellowR, kYellowG, kYellowB);  // DIT LED 1
      SetPixel(6, kYellowR, kYellowG, kYellowB);  // DIT LED 2
    }

    // Word gap indicator (center LED)
    const bool paddles_quiet = !activity.dit_active && !activity.dah_active;
    const bool word_gap_complete =
        paddles_quiet && (activity.last_transition_us != 0) &&
        (static_cast<uint64_t>(now_us - activity.last_transition_us) >
         static_cast<uint64_t>(word_gap_timeout_us_));

    if (word_gap_complete) {
      SetPixel(3, kGreenR, kGreenG, kGreenB);  // Green = word gap complete
    } else {
      SetPixel(3, kRedR, kRedG, kRedB);  // Red = still transmitting
    }
  } else {
    // Fallback for smaller LED counts (graceful degradation)
    if (led_count_ >= 1 && activity.dah_active) {
      SetPixel(0, kYellowR, kYellowG, kYellowB);  // DASH
    }
    if (led_count_ >= 2 && activity.dit_active) {
      SetPixel(led_count_ - 1, kYellowR, kYellowG, kYellowB);  // DIT (last LED)
    }
  }
}

void DiagnosticsSubsystem::UpdateWifiAnimation(int64_t now_us) {
  // Throttle animation steps
  if (animation_last_step_us_ != 0 &&
      static_cast<uint32_t>(now_us - animation_last_step_us_) < animation_step_us_) {
    return;  // Keep current frame
  }
  animation_last_step_us_ = now_us;

  ClearFrameBuffer();

  if (led_count_ == 0) {
    wifi_animation_active_ = false;
    return;
  }

  // Progressive blue fill animation
  const size_t active_idx = animation_step_ % led_count_;
  for (size_t idx = 0; idx <= active_idx && idx < led_count_; ++idx) {
    SetPixel(idx, kBlueR, kBlueG, kBlueB);
  }

  ++animation_step_;

  // Animation complete after 3 cycles
  if (animation_step_ >= led_count_ * kAnimationCycles) {
    wifi_animation_active_ = false;
    ESP_LOGI(kLogTag, "WiFi animation complete");
    ApplyBaseFrame(now_us);  // Return to paddle activity
  }
}

void DiagnosticsSubsystem::Render() {
  // Copy frame buffer to LED hardware
  for (size_t idx = 0; idx < led_count_; ++idx) {
    const Color& px = frame_[idx];
    led_driver_.SetPixel(idx, px.r, px.g, px.b);
  }
  led_driver_.Refresh();
}

void DiagnosticsSubsystem::SetPixel(size_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= led_count_) {
    return;
  }
  frame_[index] = Color{r, g, b};
}

void DiagnosticsSubsystem::ClearFrameBuffer() {
  for (size_t idx = 0; idx < led_count_; ++idx) {
    frame_[idx] = Color{};
  }
}

void DiagnosticsSubsystem::UpdateRainbowAnimation(int64_t now_us) {
  ClearFrameBuffer();

  // Initialize timestamp on first call
  if (rainbow_last_update_us_ == 0) {
    rainbow_last_update_us_ = now_us;
  }

  // Calculate elapsed time since last update
  const int64_t elapsed_us = now_us - rainbow_last_update_us_;
  rainbow_last_update_us_ = now_us;

  // Update hue angle based on elapsed time (360 degrees over 3 seconds)
  const float hue_increment = (360.0f * elapsed_us) / kRainbowCycleDurationUs;
  rainbow_hue_ += hue_increment;

  // Wrap hue to 0-360 range
  if (rainbow_hue_ >= 360.0f) {
    rainbow_hue_ -= 360.0f;
  }

  // Calculate saturation and value for 50% brightness
  // HSV: Hue varies (0-360), Saturation=100%, Value=50% (for kRainbowBrightness=128)
  constexpr float saturation = 100.0f;  // Full saturation for vivid colors
  const float value = (kRainbowBrightness / 255.0f) * 100.0f;  // Brightness percentage

  // Apply same color to all LEDs
  uint8_t r, g, b;
  HsvToRgb(rainbow_hue_, saturation, value, r, g, b);

  for (size_t idx = 0; idx < led_count_; ++idx) {
    SetPixel(idx, r, g, b);
  }
}

void DiagnosticsSubsystem::HsvToRgb(float hue, float saturation, float value, uint8_t& r, uint8_t& g, uint8_t& b) {
  // Standard HSV to RGB conversion algorithm
  // Reference: https://en.wikipedia.org/wiki/HSL_and_HSV#HSV_to_RGB

  // Normalize inputs to [0, 1] range
  hue = hue / 360.0f;              // 0-360° → 0-1
  saturation = saturation / 100.0f;  // 0-100% → 0-1
  value = value / 100.0f;            // 0-100% → 0-1

  if (saturation == 0.0f) {
    // Achromatic (gray)
    const uint8_t gray = static_cast<uint8_t>(value * 255.0f);
    r = g = b = gray;
    return;
  }

  // Calculate sector (0-5)
  const float h = hue * 6.0f;
  const int sector = static_cast<int>(h);
  const float fractional = h - sector;

  // Calculate RGB components
  const float p = value * (1.0f - saturation);
  const float q = value * (1.0f - saturation * fractional);
  const float t = value * (1.0f - saturation * (1.0f - fractional));

  float r_f, g_f, b_f;
  switch (sector % 6) {
    case 0: r_f = value; g_f = t;     b_f = p;     break;  // Red → Yellow
    case 1: r_f = q;     g_f = value; b_f = p;     break;  // Yellow → Green
    case 2: r_f = p;     g_f = value; b_f = t;     break;  // Green → Cyan
    case 3: r_f = p;     g_f = q;     b_f = value; break;  // Cyan → Blue
    case 4: r_f = t;     g_f = p;     b_f = value; break;  // Blue → Magenta
    case 5: r_f = value; g_f = p;     b_f = q;     break;  // Magenta → Red
    default: r_f = g_f = b_f = 0.0f;                        // Should never happen
  }

  // Convert to 8-bit RGB (0-255)
  r = static_cast<uint8_t>(r_f * 255.0f);
  g = static_cast<uint8_t>(g_f * 255.0f);
  b = static_cast<uint8_t>(b_f * 255.0f);
}

void DiagnosticsSubsystem::ApplyConfig(const config::DeviceConfig& device_config) {
  // TODO: Add runtime-changeable diagnostic parameters
  // Potential future parameters:
  // - LED brightness
  // - Word gap timeout
  // - Animation speed
  (void)device_config;  // Suppress unused parameter warning
}

}  // namespace diagnostics_subsystem
