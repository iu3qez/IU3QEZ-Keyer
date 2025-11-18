#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

extern "C" {
#include "esp_err.h"
#include "driver/i2c_types.h"
#include "driver/i2s_types.h"
#include "esp_codec_dev.h"
}

namespace audio {

struct CodecDriverConfig {
  i2c_master_bus_handle_t i2c_bus_handle = nullptr;
  int i2c_port = 0;
  uint8_t codec_i2c_address = 0x18;  // 7-bit ES8311 control address (0x18 << 1 used on the wire).
  i2s_chan_handle_t i2s_tx_handle = nullptr;
  int i2s_port = 0;
  bool use_mclk = false;
  uint32_t sample_rate_hz = 16000;  // 16 kHz for sidetone
  uint8_t initial_volume_percent = 50;
};

class CodecDriver {
 public:
  virtual ~CodecDriver() = default;

  virtual esp_err_t Initialize(const CodecDriverConfig& config) = 0;
  virtual esp_err_t SetMute(bool mute) = 0;
  virtual esp_err_t SetVolume(uint8_t volume_percent) = 0;
  virtual esp_err_t Write(const int16_t* data, size_t byte_count) = 0;
  virtual void Shutdown() = 0;
};

std::unique_ptr<CodecDriver> MakeEspCodecDriver();

}  // namespace audio
