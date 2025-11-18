#include "audio_subsystem/audio_subsystem.hpp"

#include "esp_log.h"

namespace audio_subsystem {

namespace {
constexpr char kLogTag[] = "audio_subsystem";
}

AudioSubsystem::AudioSubsystem() : sidetone_service_(), initialized_(false) {}

audio::SidetoneConfig AudioSubsystem::BuildSidetoneConfig(
    const config::DeviceConfig& device_config) {
  audio::SidetoneConfig config{};
  config.i2c_port = 0;
  config.i2c_sda = static_cast<gpio_num_t>(device_config.i2c.sda_gpio);
  config.i2c_scl = static_cast<gpio_num_t>(device_config.i2c.scl_gpio);
  config.codec_i2c_address = device_config.codec.i2c_address;
  config.io_expander_address = (device_config.io_expander.i2c_address > 0)
                                   ? device_config.io_expander.i2c_address
                                   : -1;
  config.io_expander_pa_pin = device_config.io_expander.pa_enable_pin;
  config.sample_rate_hz = 16000;  // 16 kHz for sidetone (good balance: low CPU, acceptable latency)
  config.i2s_port = 0;
  config.i2s_bclk = static_cast<gpio_num_t>(device_config.i2s.bclk_gpio);
  config.i2s_lrck = static_cast<gpio_num_t>(device_config.i2s.lrck_gpio);
  config.i2s_mclk = static_cast<gpio_num_t>(device_config.i2s.mclk_gpio);
  config.i2s_dout = static_cast<gpio_num_t>(device_config.i2s.dout_gpio);
  config.tone_settings.sample_rate_hz = config.sample_rate_hz;
  config.tone_settings.tone_frequency_hz = device_config.audio.sidetone_frequency_hz;
  config.tone_settings.volume_percent = device_config.audio.sidetone_volume_percent;
  config.tone_settings.fade_in_ms = device_config.audio.sidetone_fade_in_ms;
  config.tone_settings.fade_out_ms = device_config.audio.sidetone_fade_out_ms;
  return config;
}

esp_err_t AudioSubsystem::Initialize(const config::DeviceConfig& device_config) {
  ESP_LOGI(kLogTag, "Initializing audio subsystem");

  const audio::SidetoneConfig sidetone_config = BuildSidetoneConfig(device_config);
  const esp_err_t err = sidetone_service_.Initialize(sidetone_config);

  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Sidetone init failed: %s (audio disabled)", esp_err_to_name(err));
    initialized_ = false;
    return err;
  }

  initialized_ = true;

  // Enable power amplifier once at initialization (not per-tone)
  // This avoids PA turn-on delay on every CW element
  sidetone_service_.EnablePowerAmplifier(true);
  ESP_LOGI(kLogTag, "Audio subsystem initialized (freq=%u Hz, vol=%u%%, PA enabled)",
           device_config.audio.sidetone_frequency_hz, device_config.audio.sidetone_volume_percent);

  return ESP_OK;
}

esp_err_t AudioSubsystem::Start() {
  if (!initialized_) {
    ESP_LOGW(kLogTag, "Cannot start: sidetone not initialized");
    return ESP_ERR_INVALID_STATE;
  }
  return sidetone_service_.Start();
}

esp_err_t AudioSubsystem::Stop() {
  if (!initialized_) {
    return ESP_OK;  // Already stopped
  }
  sidetone_service_.Stop();
  return ESP_OK;
}

void AudioSubsystem::ApplyConfig(const config::DeviceConfig& device_config) {
  if (!initialized_) {
    ESP_LOGW(kLogTag, "Cannot apply config: sidetone not initialized");
    return;
  }

  ESP_LOGI(kLogTag, "Applying runtime config changes");

  // Apply runtime-changeable audio parameters
  const config::AudioConfig& audio_cfg = device_config.audio;
  sidetone_service_.SetFrequency(audio_cfg.sidetone_frequency_hz);
  sidetone_service_.SetVolume(audio_cfg.sidetone_volume_percent);
  sidetone_service_.SetFade(audio_cfg.sidetone_fade_in_ms, audio_cfg.sidetone_fade_out_ms);

  ESP_LOGI(kLogTag, "Audio config applied: freq=%u Hz, vol=%u%%, fade_in=%u ms, fade_out=%u ms",
           audio_cfg.sidetone_frequency_hz, audio_cfg.sidetone_volume_percent,
           audio_cfg.sidetone_fade_in_ms, audio_cfg.sidetone_fade_out_ms);
}

void AudioSubsystem::SetModeTX() {
  if (!initialized_) {
    return;
  }
  sidetone_service_.SetAudioMode(audio::SidetoneService::AudioMode::kToneGenerator);
}

void AudioSubsystem::SetModeRX() {
  if (!initialized_) {
    return;
  }
  sidetone_service_.SetAudioMode(audio::SidetoneService::AudioMode::kStreamPlayer);
}

bool AudioSubsystem::IsModeTX() const {
  if (!initialized_) {
    return true;  // Default to TX mode if not initialized
  }
  return sidetone_service_.GetAudioMode() ==
         audio::SidetoneService::AudioMode::kToneGenerator;
}

audio::AudioStreamPlayer* AudioSubsystem::GetStreamPlayer() {
  if (!initialized_) {
    return nullptr;
  }
  return &sidetone_service_.GetStreamPlayer();
}

}  // namespace audio_subsystem
