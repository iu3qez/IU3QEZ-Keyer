#pragma once

/**
 * @file sidetone_service.hpp
 * @brief Sidetone Service - Real-time audio synthesis and I2S streaming for keyer feedback
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * Manages complete audio pipeline for sidetone generation:
 * 1. **ToneGenerator**: Synthesizes sinusoidal waveforms with fade in/out envelopes
 * 2. **CodecDriver**: Controls ES8311 audio codec via I2C commands (init, start, stop, volume)
 * 3. **I2S Streaming**: Double-buffered DMA writes to I2S peripheral (256 frames/chunk, 48kHz stereo)
 * 4. **FreeRTOS Task**: Background audio pump thread with 10ms idle delay for responsiveness
 * 5. **RAII Resource Management**: Automatic cleanup of I2C bus, I2S channel, IO expander via handle wrappers
 *
 * DESIGN GOALS:
 * =============
 * - **Low Latency**: Start() → audible tone < 20ms (fade-in + task scheduling)
 * - **Clean Audio**: Envelope smoothing prevents clicks/pops during tone on/off transitions
 * - **Hot-Reload**: SetFrequency/SetVolume/SetFade apply immediately without stopping audio task
 * - **Fail-Safe**: Codec init failure is non-fatal, allows firmware to continue (silent mode)
 * - **Testability**: Dependency injection for codec driver enables unit tests without hardware
 *
 * HARDWARE DEPENDENCIES:
 * ======================
 * - **ES8311 Audio Codec** (I2C address 0x18): DAC + amplifier control
 * - **TCA9555 IO Expander** (I2C address 0x20): Power amplifier enable pin (EXI08)
 * - **I2S Peripheral**: ESP32-S3 I2S0 interface with MCLK/BCLK/LRCK/DOUT pins
 * - **Shared I2C Bus**: I2C0 bus shared between codec and IO expander (SCL=GPIO10, SDA=GPIO11)
 *
 * AUDIO PIPELINE:
 * ===============
 * ```
 * ToneGenerator::Generate(256 frames)
 *   ↓ [Sinusoidal synthesis + envelope application]
 * AudioChunk[kSamplesPerChunk] (stereo interleaved int16_t)
 *   ↓ [Double-buffering: buffers_[next_buffer_index_]]
 * i2s_channel_write(i2s_tx_handle_, chunk, timeout)
 *   ↓ [DMA transfer to I2S peripheral]
 * ES8311 DAC → Analog audio output
 * ```
 *
 * THREAD SAFETY:
 * ==============
 * - **Initialize/Deinitialize**: Must be called from main task context (not thread-safe)
 * - **Start/Stop**: Thread-safe via FreeRTOS task synchronization (vTaskDelete)
 * - **SetFrequency/SetVolume/SetFade**: Hot-reload safe (ToneGenerator uses atomic updates)
 * - **AudioTask**: Runs in dedicated FreeRTOS task (priority 5, 4KB stack)
 *
 * RESOURCE LIFECYCLE (RAII):
 * ===========================
 * RAII handle wrappers ensure automatic cleanup on failure or Deinitialize():
 * - **I2cBusHandle**: Calls i2c_del_master_bus() on destruction
 * - **IoExpanderHandle**: Calls esp_io_expander_del() on destruction
 * - **I2sChannelHandle**: Calls i2s_del_channel() on destruction
 *
 * TIMING CONSTANTS:
 * =================
 * - kFramesPerChunk (256): Tradeoff between latency and CPU efficiency (5.3ms @ 48kHz)
 * - kCodecChannelCount (2): Stereo output (left=right for sidetone)
 * - kCodecBufferCount (2): Double-buffering for DMA (prevents underruns)
 * - Audio Task Delay: 10ms idle (responsive), 5ms error retry (prevents log spam)
 *
 * CONFIGURATION STRUCT (SidetoneConfig):
 * ======================================
 * - I2C: port (0), SDA/SCL GPIOs, codec address (0x18)
 * - IO Expander: I2C address (0x20), PA enable pin (EXI08)
 * - I2S: port (0), BCLK/LRCK/MCLK/DOUT GPIOs
 * - Audio: sample_rate_hz (48000), tone_settings (freq, volume, fade)
 *
 * USAGE EXAMPLE:
 * ==============
 * @code
 * audio::SidetoneConfig config = BuildSidetoneConfig(device_config);
 * audio::SidetoneService sidetone;
 *
 * sidetone.Initialize(config);  // Init codec, I2S, start audio task
 * sidetone.Start();              // Enable tone generation
 * vTaskDelay(pdMS_TO_TICKS(100)); // 100ms tone burst
 * sidetone.Stop();               // Silence tone (fade-out)
 *
 * // Hot-reload example:
 * sidetone.SetFrequency(700);  // Change frequency without restart
 * sidetone.SetVolume(75);      // Adjust volume without restart
 * @endcode
 *
 * ERROR HANDLING:
 * ===============
 * - Codec init failure: Log warning, continue in silent mode (IsInitialized() → true)
 * - I2S write timeout: Log error every 5ms, auto-retry (prevents infinite blocking)
 * - PA enable failure: Non-fatal, sidetone works without PA (lower volume)
 *
 * DEPENDENCIES:
 * =============
 * - ToneGenerator: Sinusoidal synthesis with envelope (components/audio_subsystem/include/audio/tone_generator.hpp)
 * - CodecDriver: ES8311 control interface (components/audio_subsystem/include/audio/codec_driver.hpp)
 * - RAII Handles: Resource wrappers (components/audio_subsystem/include/audio/raii_handles.hpp)
 * - ESP-IDF I2C: i2c_master driver (components/esp_driver_i2c)
 * - ESP-IDF I2S: i2s_std driver (components/esp_driver_i2s)
 * - esp_io_expander: TCA9555 driver (managed_components/espressif__esp_io_expander)
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "audio/audio_stream_player.hpp"
#include "audio/codec_driver.hpp"
#include "audio/raii_handles.hpp"
#include "audio/tone_generator.hpp"

extern "C" {
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace audio {

struct SidetoneConfig {
  int i2c_port = 0;
  gpio_num_t i2c_sda = GPIO_NUM_NC;
  gpio_num_t i2c_scl = GPIO_NUM_NC;
  uint8_t codec_i2c_address = 0x18;
  int io_expander_address = -1;
  int8_t io_expander_pa_pin = -1;
  gpio_num_t amp_enable_gpio = GPIO_NUM_NC;
  int i2s_port = 0;
  gpio_num_t i2s_bclk = GPIO_NUM_NC;
  gpio_num_t i2s_lrck = GPIO_NUM_NC;
  gpio_num_t i2s_mclk = GPIO_NUM_NC;
  gpio_num_t i2s_dout = GPIO_NUM_NC;
  uint32_t sample_rate_hz = 16000;  // 16 kHz for sidetone
  ToneGeneratorSettings tone_settings{};
};

class SidetoneService {
 public:
  SidetoneService() = default;
  ~SidetoneService();

  SidetoneService(const SidetoneService&) = delete;
  SidetoneService& operator=(const SidetoneService&) = delete;

  esp_err_t Initialize(const SidetoneConfig& config);
  void Deinitialize();

  esp_err_t Start();
  void Stop();

  bool IsInitialized() const { return initialized_; }
  bool IsActive() const;

  void SetFrequency(uint16_t frequency_hz);
  void SetVolume(uint8_t volume_percent);
  void SetFade(uint16_t fade_in_ms, uint16_t fade_out_ms);

  /**
   * @brief Enable/disable power amplifier (should be called at subsystem level, not per-tone).
   * @param enable true = enable PA, false = disable PA
   */
  void EnablePowerAmplifier(bool enable);

  /**
   * @brief Audio output mode for sidetone service.
   */
  enum class AudioMode : uint8_t {
    kToneGenerator = 0,  // Local sidetone (TX mode)
    kStreamPlayer = 1,   // Remote audio stream (RX mode)
  };

  /**
   * @brief Switch audio output mode.
   * @param mode kToneGenerator for local TX, kStreamPlayer for remote RX.
   */
  void SetAudioMode(AudioMode mode);

  /**
   * @brief Get current audio output mode.
   */
  AudioMode GetAudioMode() const { return audio_mode_; }

  /**
   * @brief Get audio stream player reference (for RemoteCwClient injection).
   */
  AudioStreamPlayer& GetStreamPlayer() { return stream_player_; }

  static constexpr uint32_t kFramesPerChunk = 256;
  static constexpr uint8_t kCodecChannelCount = 2;
  static constexpr size_t kCodecBufferCount = 2;

  void SetCodecDriverForTesting(std::unique_ptr<CodecDriver> driver);
  esp_err_t PumpAudioOnceForTest();

 private:
  static void AudioTaskThunk(void* arg);
  void AudioTask();
  esp_err_t PumpAudioChunk();
  esp_err_t ConfigureI2c();
  esp_err_t ConfigureIoExpander();
  esp_err_t ConfigureI2s();
  esp_err_t ConfigureCodec();
  uint32_t FramesPerChunk() const;

  SidetoneConfig config_{};
  ToneGenerator generator_{};

  bool initialized_ = false;
  bool running_ = false;
  bool audio_started_ = false;  // True after first Start() - keeps codec pumping audio
  bool pa_enabled_ = false;
  bool amp_gpio_configured_ = false;
  bool codec_driver_injected_for_test_ = false;

  AudioMode audio_mode_ = AudioMode::kToneGenerator;
  AudioStreamPlayer stream_player_{};

  // RAII handles for automatic resource cleanup (Task 9.3)
  I2cBusHandle i2c_bus_handle_;
  IoExpanderHandle io_expander_handle_;
  I2sChannelHandle i2s_tx_handle_;
  TaskHandle_t task_handle_ = nullptr;
  std::unique_ptr<CodecDriver> codec_driver_;

  static constexpr size_t kSamplesPerChunk = kFramesPerChunk * kCodecChannelCount;
  using AudioChunk = std::array<int16_t, kSamplesPerChunk>;
  std::array<AudioChunk, kCodecBufferCount> audio_buffers_{};
  size_t next_buffer_index_ = 0;
  size_t bytes_per_chunk_ = kSamplesPerChunk * sizeof(int16_t);
};

}  // namespace audio
