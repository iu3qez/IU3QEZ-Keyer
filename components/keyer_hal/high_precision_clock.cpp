#include "hal/high_precision_clock.hpp"

extern "C" {
#include "esp_timer.h"
}

namespace hal {

esp_err_t HighPrecisionClock::Initialize() {
  // ESP-IDF initializes esp_timer during startup; provide hook for symmetry.
  return ESP_OK;
}

int64_t HighPrecisionClock::NowMicros() {
  return esp_timer_get_time();
}

int64_t HighPrecisionClock::NowMillis() {
  return esp_timer_get_time() / 1000;
}

}  // namespace hal
