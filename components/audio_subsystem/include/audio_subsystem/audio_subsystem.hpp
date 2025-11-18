#pragma once

/**
 * @file audio_subsystem.hpp
 * @brief Audio Subsystem - Sidetone service integration layer
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * Encapsulates audio hardware initialization and configuration,
 * separating audio concerns from application orchestration logic.
 *
 * RESPONSIBILITIES:
 * - Build sidetone configuration from DeviceConfig (I2C, I2S, codec pins)
 * - Initialize and manage SidetoneService lifecycle
 * - Provide runtime access to sidetone controls (start/stop/volume/freq)
 *
 * USAGE PATTERN:
 * ```
 * AudioSubsystem audio;
 * audio.Initialize(device_config);  // Setup codec + I2S + sidetone
 * 
 * if (audio.IsReady() && device_config.audio.sidetone_enabled) {
 *   audio.Start();
 * }
 * ```
 */

#include "audio/sidetone_service.hpp"
#include "config/device_config.hpp"
#include "esp_err.h"

namespace audio {
class AudioStreamPlayer;
}

namespace audio_subsystem {

/**
 * @brief Manages sidetone audio subsystem initialization and lifecycle.
 */
class AudioSubsystem {
 public:
  AudioSubsystem();
  ~AudioSubsystem() = default;

  /**
   * @brief Initialize sidetone service with device configuration.
   * @param device_config Device configuration (I2C, I2S, codec settings).
   * @return ESP_OK on success, error code if codec/I2S init fails (non-fatal).
   */
  esp_err_t Initialize(const config::DeviceConfig& device_config);

  /**
   * @brief Check if sidetone initialized successfully.
   */
  bool IsReady() const { return initialized_; }

  /**
   * @brief Start sidetone audio output.
   */
  esp_err_t Start();

  /**
   * @brief Stop sidetone audio output.
   */
  esp_err_t Stop();

  /**
   * @brief Get direct reference to sidetone service for runtime control.
   */
  audio::SidetoneService& GetService() { return sidetone_service_; }

  /**
   * @brief Apply runtime configuration changes (hot-reload).
   *
   * Updates sidetone parameters that can be changed without hardware reinitialization:
   * - sidetone_frequency_hz
   * - sidetone_volume_percent
   * - sidetone_fade_in_ms, sidetone_fade_out_ms
   * - sidetone_enabled (start/stop)
   *
   * Hardware parameters (I2C, I2S pins) require device reset.
   *
   * @param device_config Updated device configuration
   */
  void ApplyConfig(const config::DeviceConfig& device_config);

  /**
   * @brief Switch to TX mode (local sidetone generation).
   *
   * Called by KeyingSubsystem when paddle becomes active.
   */
  void SetModeTX();

  /**
   * @brief Switch to RX mode (remote audio stream playback).
   *
   * Called by KeyingSubsystem when paddle idle and remote streaming active.
   */
  void SetModeRX();

  /**
   * @brief Get current audio mode.
   * @return true if TX mode (ToneGenerator), false if RX mode (StreamPlayer).
   */
  bool IsModeTX() const;

  /**
   * @brief Get audio stream player reference for RemoteCwClient.
   * @return Pointer to AudioStreamPlayer (nullptr if not initialized).
   */
  audio::AudioStreamPlayer* GetStreamPlayer();

 private:
  /**
   * @brief Build sidetone config from device config.
   */
  static audio::SidetoneConfig BuildSidetoneConfig(const config::DeviceConfig& device_config);

  audio::SidetoneService sidetone_service_;
  bool initialized_;
};

}  // namespace audio_subsystem
