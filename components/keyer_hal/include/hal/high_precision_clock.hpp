#pragma once

#include <cstdint>

extern "C" {
#include "esp_err.h"
}

namespace hal {

class HighPrecisionClock {
 public:
  static esp_err_t Initialize();
  static int64_t NowMicros();
  static int64_t NowMillis();
};

}  // namespace hal
