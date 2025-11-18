#include "audio/codec_driver.hpp"

#include <memory>

extern "C" {
#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_types.h"
#include "esp_io_expander.h"
}

namespace audio {

namespace {
constexpr uint8_t kStereoChannelCount = 2;          // ES8311 outputs stereo DAC lanes.
constexpr uint8_t kBitsPerSample = 16;              // ES8311 configured for 16-bit PCM output.
constexpr uint16_t kStereoChannelMask =
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);  // Enable both audio channels.
constexpr int kDefaultMclkMultiple = 0;             // 0 -> codec derives MCLK as sample_rate * 256.
constexpr float kCodecDacVoltage = 3.3f;           // ES8311 DAC supply voltage.
constexpr float kPaSupplyVoltage = 5.0f;           // External PA supply for gain calculations.

class EspCodecDriver final : public CodecDriver {
 public:
  EspCodecDriver() = default;
  ~EspCodecDriver() override { Shutdown(); }

  esp_err_t Initialize(const CodecDriverConfig& config) override {
    if (initialized_) {
      return ESP_ERR_INVALID_STATE;
    }

    config_ = config;

    if (config_.codec_i2c_address >= 0x80) {
      Shutdown();
      return ESP_ERR_INVALID_ARG;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = static_cast<uint8_t>(config_.i2c_port),
        .addr = static_cast<uint8_t>(config_.codec_i2c_address << 1),
        .bus_handle = config_.i2c_bus_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if_ == nullptr) {
      Shutdown();
      return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = static_cast<uint8_t>(config_.i2s_port),
        .rx_handle = nullptr,
        .tx_handle = config_.i2s_tx_handle,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if_ == nullptr) {
      Shutdown();
      return ESP_FAIL;
    }

    gpio_if_ = audio_codec_new_gpio();
    if (gpio_if_ == nullptr) {
      Shutdown();
      return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if_,
        .gpio_if = gpio_if_,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = config_.use_mclk,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {.pa_voltage = kPaSupplyVoltage,
                    .codec_dac_voltage = kCodecDacVoltage,
                    .pa_gain = 0.0f},
        .no_dac_ref = false,
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };

    codec_if_ = es8311_codec_new(&es8311_cfg);
    if (codec_if_ == nullptr) {
      Shutdown();
      return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if_,
        .data_if = data_if_,
    };

    codec_handle_ = esp_codec_dev_new(&dev_cfg);
    if (codec_handle_ == nullptr) {
      Shutdown();
      return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = kBitsPerSample,
        .channel = kStereoChannelCount,
        .channel_mask = kStereoChannelMask,
        .sample_rate = config_.sample_rate_hz,
        .mclk_multiple = kDefaultMclkMultiple,
    };

    if (esp_codec_dev_open(codec_handle_, &sample_info) != ESP_CODEC_DEV_OK) {
      Shutdown();
      return ESP_FAIL;
    }

    if (esp_codec_dev_set_out_mute(codec_handle_, true) != ESP_CODEC_DEV_OK) {
      Shutdown();
      return ESP_FAIL;
    }

    if (esp_codec_dev_set_out_vol(codec_handle_, config_.initial_volume_percent) != ESP_CODEC_DEV_OK) {
      Shutdown();
      return ESP_FAIL;
    }

    initialized_ = true;
    return ESP_OK;
  }

  esp_err_t SetMute(bool mute) override {
    if (!initialized_ || codec_handle_ == nullptr) {
      return ESP_ERR_INVALID_STATE;
    }
    return (esp_codec_dev_set_out_mute(codec_handle_, mute) == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
  }

  esp_err_t SetVolume(uint8_t volume_percent) override {
    if (!initialized_ || codec_handle_ == nullptr) {
      return ESP_ERR_INVALID_STATE;
    }
    return (esp_codec_dev_set_out_vol(codec_handle_, volume_percent) == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
  }

  esp_err_t Write(const int16_t* data, size_t byte_count) override {
    if (!initialized_ || codec_handle_ == nullptr) {
      return ESP_ERR_INVALID_STATE;
    }
    if (data == nullptr || byte_count == 0) {
      return ESP_ERR_INVALID_ARG;
    }
    return (esp_codec_dev_write(codec_handle_, const_cast<int16_t*>(data), static_cast<int>(byte_count)) ==
            ESP_CODEC_DEV_OK)
               ? ESP_OK
               : ESP_FAIL;
  }

  void Shutdown() override {
    if (codec_handle_ != nullptr) {
      esp_codec_dev_close(codec_handle_);
      esp_codec_dev_delete(codec_handle_);
      codec_handle_ = nullptr;
    }
    if (codec_if_ != nullptr) {
      codec_if_->close(codec_if_);
      audio_codec_delete_codec_if(codec_if_);
      codec_if_ = nullptr;
    }
    if (data_if_ != nullptr) {
      if (data_if_->is_open != nullptr && data_if_->is_open(data_if_)) {
        data_if_->close(data_if_);
      }
      audio_codec_delete_data_if(data_if_);
      data_if_ = nullptr;
    }
    if (ctrl_if_ != nullptr) {
      if (ctrl_if_->is_open != nullptr && ctrl_if_->is_open(ctrl_if_)) {
        ctrl_if_->close(ctrl_if_);
      } else if (ctrl_if_->close != nullptr) {
        ctrl_if_->close(ctrl_if_);
      }
      audio_codec_delete_ctrl_if(ctrl_if_);
      ctrl_if_ = nullptr;
    }
    if (gpio_if_ != nullptr) {
      audio_codec_delete_gpio_if(gpio_if_);
      gpio_if_ = nullptr;
    }
    initialized_ = false;
  }

 private:
  CodecDriverConfig config_{};
  const audio_codec_ctrl_if_t* ctrl_if_ = nullptr;
  const audio_codec_data_if_t* data_if_ = nullptr;
  const audio_codec_gpio_if_t* gpio_if_ = nullptr;
  const audio_codec_if_t* codec_if_ = nullptr;
  esp_codec_dev_handle_t codec_handle_ = nullptr;
  bool initialized_ = false;
};

}  // namespace

std::unique_ptr<CodecDriver> MakeEspCodecDriver() {
  return std::make_unique<EspCodecDriver>();
}

}  // namespace audio
