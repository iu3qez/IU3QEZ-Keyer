#include "audio/sidetone_service.hpp"

#include <algorithm>
#include <array>
#include <memory>

extern "C" {
#include "esp_check.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_log.h"
}

#include "audio/codec_driver.hpp"
#include "audio/tone_generator.hpp"

namespace audio {

namespace {
constexpr char kLogTag[] = "SidetoneService";  // Logging category for sidetone service events.

// Task 9.8.2: Audio task timing constants - balanced for latency vs CPU usage
// kAudioTaskDelayIdleMs: Delay when sidetone is inactive/muted (PumpAudioChunk returns ESP_ERR_INVALID_STATE)
// Rationale: 10ms keeps task responsive to Start() commands while avoiding busy-wait.
// At 48kHz with 256-frame chunks, active mode pushes audio every 5.3ms, so 10ms idle is reasonable.
// CPU impact: When muted, task yields CPU 100 times/sec (negligible overhead, no audio processing).
constexpr uint32_t kAudioTaskDelayIdleMs = 10;

// kAudioTaskDelayErrorMs: Backoff delay after codec I2C/I2S write failures
// Rationale: 5ms allows transient I2C bus recovery without flooding logs (errors at 200Hz max).
// Shorter than idle (5ms vs 10ms) to retry quickly - most failures are transient (bus arbitration).
// If errors persist >100ms (20 retries), likely indicates hardware fault requiring power cycle.
constexpr uint32_t kAudioTaskDelayErrorMs = 5;

uint32_t PinMaskFromIndex(int8_t index) {
  if (index < 0 || index >= 32) {
    return 0;
  }
  return (1u << static_cast<uint32_t>(index));
}
}  // namespace

SidetoneService::~SidetoneService() {
  Deinitialize();
}

void SidetoneService::Deinitialize() {
  running_ = false;

  // Task cleanup (still manual as TaskHandle_t has no RAII wrapper yet)
  if (task_handle_ != nullptr) {
    vTaskDelete(task_handle_);
    task_handle_ = nullptr;
  }

  // RAII handles clean up automatically in reverse declaration order:
  // 1. i2s_tx_handle_ (disable + delete channel)
  // 2. io_expander_handle_ (delete expander)
  // 3. i2c_bus_handle_ (delete bus)
  i2s_tx_handle_.Reset();
  io_expander_handle_.Reset();
  i2c_bus_handle_.Reset();

  // Codec driver cleanup (already RAII via unique_ptr)
  if (codec_driver_ != nullptr) {
    codec_driver_->Shutdown();
    if (!codec_driver_injected_for_test_) {
      codec_driver_.reset();
    }
  }

  amp_gpio_configured_ = false;
  initialized_ = false;
  pa_enabled_ = false;
  next_buffer_index_ = 0;
}

esp_err_t SidetoneService::Initialize(const SidetoneConfig& config) {
  Deinitialize();
  config_ = config;

  generator_.Configure(config_.tone_settings);

  esp_err_t err = ConfigureI2c();
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "I2C init failed: %s", esp_err_to_name(err));
    Deinitialize();
    return err;
  }

  err = ConfigureIoExpander();
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "IO expander init failed: %s", esp_err_to_name(err));
    Deinitialize();
    return err;
  }

  err = ConfigureI2s();
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "I2S init failed: %s", esp_err_to_name(err));
    Deinitialize();
    return err;
  }

  err = ConfigureCodec();
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Codec init failed: %s", esp_err_to_name(err));
    Deinitialize();
    return err;
  }
  next_buffer_index_ = 0;

  BaseType_t created = xTaskCreatePinnedToCore(&SidetoneService::AudioTaskThunk, "sidetone_loop", 4096, this, 4,
                                               &task_handle_, tskNO_AFFINITY);
  if (created != pdPASS) {
    ESP_LOGE(kLogTag, "Failed to create audio task");
    Deinitialize();
    return ESP_ERR_NO_MEM;
  }

  initialized_ = true;
  ESP_LOGI(kLogTag, "Sidetone service initialized (sample_rate=%lu Hz, freq=%u Hz)",
           static_cast<unsigned long>(config_.sample_rate_hz),
           static_cast<unsigned>(config_.tone_settings.tone_frequency_hz));
  return ESP_OK;
}

esp_err_t SidetoneService::ConfigureI2c() {
  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = static_cast<i2c_port_t>(config_.i2c_port),
      .sda_io_num = config_.i2c_sda,
      .scl_io_num = config_.i2c_scl,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,  // Datasheet recommendation for ES8311 wiring.
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags = {.enable_internal_pullup = true},
  };

  return i2c_bus_handle_.Create(&bus_cfg);
}

esp_err_t SidetoneService::ConfigureIoExpander() {
  if (config_.io_expander_address < 0) {
    return ESP_OK;
  }
  const uint32_t address = static_cast<uint32_t>(config_.io_expander_address);
  esp_err_t err = io_expander_handle_.CreateTca95xx16Bit(i2c_bus_handle_.Get(), address);
  if (err != ESP_OK) {
    return err;
  }
  if (config_.io_expander_pa_pin >= 0) {
    const uint32_t mask = PinMaskFromIndex(config_.io_expander_pa_pin);
    if (mask == 0) {
      return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io_expander_handle_.Get(), mask, IO_EXPANDER_OUTPUT), kLogTag,
                        "Failed to set PA pin direction");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander_handle_.Get(), mask, 0), kLogTag,
                        "Failed to clear PA pin");
  }
  return ESP_OK;
}

esp_err_t SidetoneService::ConfigureI2s() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(config_.i2s_port),
                                                          I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  ESP_RETURN_ON_ERROR(i2s_tx_handle_.Create(&chan_cfg, i2s_tx_handle_.GetAddressOf(), nullptr), kLogTag,
                      "i2s_new_channel failed");

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config_.sample_rate_hz),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = config_.i2s_mclk,
                   .bclk = config_.i2s_bclk,
                   .ws = config_.i2s_lrck,
                   .dout = config_.i2s_dout,
                   .din = GPIO_NUM_NC,
                   .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}},
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;  // Ensure codec sees 256 * sample_rate MCLK.

  ESP_RETURN_ON_ERROR(i2s_tx_handle_.InitStdMode(&std_cfg), kLogTag,
                      "i2s_channel_init_std_mode failed");
  ESP_RETURN_ON_ERROR(i2s_tx_handle_.Enable(), kLogTag, "i2s_channel_enable failed");
  return ESP_OK;
}

esp_err_t SidetoneService::ConfigureCodec() {
  if (codec_driver_ == nullptr) {
    codec_driver_ = MakeEspCodecDriver();
    if (codec_driver_ == nullptr) {
      return ESP_ERR_NO_MEM;
    }
    codec_driver_injected_for_test_ = false;
  }

  codec_driver_->Shutdown();

  CodecDriverConfig driver_cfg = {
      .i2c_bus_handle = i2c_bus_handle_.Get(),
      .i2c_port = config_.i2c_port,
      .codec_i2c_address = config_.codec_i2c_address,
      .i2s_tx_handle = i2s_tx_handle_.Get(),
      .i2s_port = config_.i2s_port,
      .use_mclk = (config_.i2s_mclk != GPIO_NUM_NC),
      .sample_rate_hz = config_.sample_rate_hz,
      .initial_volume_percent = config_.tone_settings.volume_percent,
  };

  return codec_driver_->Initialize(driver_cfg);
}

esp_err_t SidetoneService::Start() {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

  // Unmute codec BEFORE starting generator to prevent click
  // Hardware unmute transient must settle before audio fade-in begins
  if (codec_driver_ != nullptr) {
    const esp_err_t rc = codec_driver_->SetMute(false);
    if (rc != ESP_OK) {
      ESP_LOGW(kLogTag, "Failed to unmute codec (%s)", esp_err_to_name(rc));
    }
    // Verbose debug log commented out
    // else {
    //   ESP_LOGI(kLogTag, "Codec unmuted successfully");
    // }
  }

  generator_.Start();
  running_ = true;
  audio_started_ = true;  // Mark codec as active - keeps pumping audio even when silent

  next_buffer_index_ = 0;

  // Verbose debug log commented out
  // ESP_LOGI(kLogTag, "Sidetone started");
  return ESP_OK;
}

void SidetoneService::Stop() {
  if (!initialized_) {
    return;
  }
  generator_.Stop();  // Initiates fade out (pending_stop_=true)
  running_ = false;
  // PA control moved to AudioSubsystem level (no on/off per tone)
  // Codec mute removed: fade out already brings volume to 0, hardware mute causes click
  // Verbose debug log commented out
  // ESP_LOGI(kLogTag, "Sidetone stopped");
}

bool SidetoneService::IsActive() const {
  return running_;
}

void SidetoneService::SetFrequency(uint16_t frequency_hz) {
  generator_.SetFrequency(frequency_hz);
}

void SidetoneService::SetVolume(uint8_t volume_percent) {
  generator_.SetVolume(volume_percent);
  if (codec_driver_ != nullptr) {
    const esp_err_t rc = codec_driver_->SetVolume(volume_percent);
    if (rc != ESP_OK) {
      ESP_LOGW(kLogTag, "Failed to propagate volume to codec (%s)", esp_err_to_name(rc));
    }
  }
}

void SidetoneService::SetFade(uint16_t fade_in_ms, uint16_t fade_out_ms) {
  generator_.SetFade(fade_in_ms, fade_out_ms);
  config_.tone_settings.fade_in_ms = fade_in_ms;
  config_.tone_settings.fade_out_ms = fade_out_ms;
}

void SidetoneService::AudioTaskThunk(void* arg) {
  auto* self = static_cast<SidetoneService*>(arg);
  if (self != nullptr) {
    self->AudioTask();
  }
  vTaskDelete(nullptr);
}

void SidetoneService::AudioTask() {
  while (true) {
    const esp_err_t rc = PumpAudioChunk();
    if (rc == ESP_ERR_INVALID_STATE) {
      vTaskDelay(pdMS_TO_TICKS(kAudioTaskDelayIdleMs));
    } else if (rc != ESP_OK) {
      ESP_LOGW(kLogTag, "Codec write failed: %s", esp_err_to_name(rc));
      vTaskDelay(pdMS_TO_TICKS(kAudioTaskDelayErrorMs));
    }
  }
}

void SidetoneService::EnablePowerAmplifier(bool enable) {
  if (pa_enabled_ == enable) {
    return;
  }

  if (io_expander_handle_.IsValid() && config_.io_expander_pa_pin >= 0) {
    const uint32_t mask = PinMaskFromIndex(config_.io_expander_pa_pin);
    if (mask != 0) {
      esp_err_t err = esp_io_expander_set_level(io_expander_handle_.Get(), mask, enable ? 1 : 0);
      if (err != ESP_OK) {
        ESP_LOGW(kLogTag, "Failed to %s PA via IO expander pin %d: %s",
                 enable ? "enable" : "disable",
                 config_.io_expander_pa_pin,
                 esp_err_to_name(err));
      } else {
        ESP_LOGI(kLogTag, "Power amplifier %s (IO expander pin %d)",
                 enable ? "enabled" : "disabled",
                 config_.io_expander_pa_pin);
      }
    }
  } else if (config_.amp_enable_gpio != GPIO_NUM_NC) {
    if (!amp_gpio_configured_) {
      gpio_config_t cfg = {
          .pin_bit_mask = (1ULL << static_cast<uint32_t>(config_.amp_enable_gpio)),
          .mode = GPIO_MODE_OUTPUT,
          .pull_up_en = GPIO_PULLUP_DISABLE,
          .pull_down_en = GPIO_PULLDOWN_DISABLE,
          .intr_type = GPIO_INTR_DISABLE,
      };
      if (gpio_config(&cfg) == ESP_OK) {
        amp_gpio_configured_ = true;
        ESP_LOGI(kLogTag, "Configured amp_enable_gpio=%d", static_cast<int>(config_.amp_enable_gpio));
      }
    }
    if (amp_gpio_configured_) {
      gpio_set_level(config_.amp_enable_gpio, enable ? 1 : 0);
      ESP_LOGI(kLogTag, "Power amplifier %s (GPIO %d)",
               enable ? "enabled" : "disabled",
               static_cast<int>(config_.amp_enable_gpio));
    }
  } else {
    if (enable) {
      ESP_LOGW(kLogTag, "Power amplifier control not configured");
    }
  }

  pa_enabled_ = enable;
}

uint32_t SidetoneService::FramesPerChunk() const {
  return kFramesPerChunk;
}

void SidetoneService::SetCodecDriverForTesting(std::unique_ptr<CodecDriver> driver) {
  codec_driver_ = std::move(driver);
  codec_driver_injected_for_test_ = (codec_driver_ != nullptr);
}

esp_err_t SidetoneService::PumpAudioOnceForTest() {
  return PumpAudioChunk();
}

esp_err_t SidetoneService::PumpAudioChunk() {
  if (!initialized_ || codec_driver_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  // Keep pumping audio after first Start() - codec stays active even when silent
  // This eliminates init delays and ensures fade-out completes before FSM moves on
  if (!audio_started_) {
    return ESP_ERR_INVALID_STATE;
  }

  AudioChunk& buffer = audio_buffers_[next_buffer_index_];

  if (audio_mode_ == AudioMode::kToneGenerator) {
    // TX mode: Generate local sidetone
    generator_.Fill(buffer.data(), FramesPerChunk());
  } else {
    // RX mode: Read from remote audio stream
    const size_t frames_read = stream_player_.ReadStereoFrames(buffer.data(), FramesPerChunk());

    // If underrun, fill remaining with silence
    if (frames_read < FramesPerChunk()) {
      std::fill(buffer.begin() + frames_read * kCodecChannelCount,
                buffer.end(), 0);
    }
  }

  const esp_err_t rc = codec_driver_->Write(buffer.data(), bytes_per_chunk_);
  if (rc == ESP_OK) {
    next_buffer_index_ = (next_buffer_index_ + 1U) % audio_buffers_.size();
  }
  return rc;
}

void SidetoneService::SetAudioMode(AudioMode mode) {
  if (audio_mode_ == mode) {
    return;  // Already in this mode
  }

  ESP_LOGI("SidetoneService", "Switching audio mode: %s → %s",
           (audio_mode_ == AudioMode::kToneGenerator) ? "ToneGenerator" : "StreamPlayer",
           (mode == AudioMode::kToneGenerator) ? "ToneGenerator" : "StreamPlayer");

  audio_mode_ = mode;

  if (mode == AudioMode::kStreamPlayer) {
    // Entering RX mode: reset stream player buffer
    stream_player_.Reset();
  }
}

}  // namespace audio
