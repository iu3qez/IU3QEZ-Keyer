#pragma once

// Uncomment to use polling mode instead of GPIO interrupts for paddle input
// Polling mode: Reads GPIO at 50Hz (20ms intervals) instead of ISR on every edge
// Use cases: Hardware with excessive contact bounce (300+ events/press), debugging ISR issues
// Trade-off: Slightly higher CPU usage, but more predictable timing and no ISR queue overflow
#define PADDLE_USE_POLLING

#include <cstdint>

extern "C" {
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
}

namespace hal {

enum class PaddleLine : uint8_t {
  kDit = 0,
  kDah = 1,
  kKey = 2,
};

struct PaddleEvent {
  PaddleLine line = PaddleLine::kDit;
  bool active = false;
  int64_t timestamp_us = 0;
  uint32_t raw_level = 0;
};

struct PaddlePinConfig {
  gpio_num_t gpio = GPIO_NUM_NC;
  bool active_low = true;
  bool pull_up = true;
  bool pull_down = false;
};

struct PaddleHalConfig {
  PaddlePinConfig dit;
  PaddlePinConfig dah;
  PaddlePinConfig key;
};

using PaddleEventCallback = void (*)(const PaddleEvent&, void* context);

class PaddleHal {
 public:
  PaddleHal() = default;
  ~PaddleHal();

  PaddleHal(const PaddleHal&) = delete;
  PaddleHal& operator=(const PaddleHal&) = delete;

  esp_err_t Initialize(const PaddleHalConfig& config,
                       PaddleEventCallback callback,
                       void* context);
  void Shutdown();

  bool IsInitialized() const { return initialized_; }
  bool HasConfiguredPins() const;

#ifdef PADDLE_USE_POLLING
  // Polling-mode: Read GPIO pins and generate events for detected edges
  // Call this periodically (e.g., every 20ms from main loop)
  void Poll();
#endif

 private:
  struct GpioIsrContext {
    PaddleHal* self = nullptr;
    PaddleLine line = PaddleLine::kDit;
  };

  static void IRAM_ATTR HandleGpioInterrupt(void* arg);
  void IRAM_ATTR DispatchEdge(PaddleLine line) const;

  esp_err_t ConfigurePin(const PaddlePinConfig& pin_config, PaddleLine line);
  const PaddlePinConfig& PinConfigFor(PaddleLine line) const;

  PaddleHalConfig config_{};
  PaddleEventCallback callback_ = nullptr;
  void* callback_context_ = nullptr;
  bool initialized_ = false;
  bool pin_configured_[3]{};
  GpioIsrContext gpio_contexts_[3]{};

#ifdef PADDLE_USE_POLLING
  // Previous GPIO levels for edge detection in polling mode
  int last_dit_level_ = -1;
  int last_dah_level_ = -1;
  int last_key_level_ = -1;
#endif
};

}  // namespace hal
