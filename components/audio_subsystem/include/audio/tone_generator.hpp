#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace audio {

struct ToneGeneratorSettings {
  uint32_t sample_rate_hz = 16000;  // 16 kHz for sidetone
  uint16_t tone_frequency_hz = 600;
  uint8_t volume_percent = 70;
  uint16_t fade_in_ms = 5;
  uint16_t fade_out_ms = 5;
};

class ToneGenerator {
 public:
  ToneGenerator() = default;

  void Configure(const ToneGeneratorSettings& settings);
  void Start();
  void Stop();
  bool IsActive() const;

  void SetFrequency(uint16_t frequency_hz);
  uint16_t Frequency() const;

  void SetVolume(uint8_t percent);
  uint8_t Volume() const;

  void SetFade(uint16_t fade_in_ms, uint16_t fade_out_ms);
  uint16_t FadeInMs() const;
  uint16_t FadeOutMs() const;

  static constexpr size_t kLutSize = 1024;

  void Fill(int16_t* stereo_buffer, size_t frames);

 private:
  enum class State {
    kSilent = 0,  // Generating at volume 0 (codec stays active)
    kFadeIn,
    kPlaying,
    kFadeOut,
  };

  void UpdatePhaseStep();
  size_t MillisecondsToSamples(uint16_t duration_ms) const;
  int16_t ConvertToSample(float value) const;
  float SampleFromLut(float phase_index) const;

  ToneGeneratorSettings settings_{};
  State state_ = State::kSilent;
  bool pending_stop_ = false;

  size_t fade_in_samples_ = 0;
  size_t fade_out_samples_ = 0;
  size_t fade_position_ = 0;

  float phase_ = 0.0f;
  float phase_step_ = 0.0f;
  int16_t amplitude_ = 0;

  mutable std::mutex mutex_;  // Protects state machine and audio generation variables

  static const std::array<float, kLutSize> kSineLut;
};

}  // namespace audio
