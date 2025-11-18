#include "audio/tone_generator.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace audio {

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;  // One full rotation in radians.

int16_t VolumePercentToAmplitude(uint8_t percent) {
  if (percent == 0) {
    return 0;
  }
  const int32_t kMaxSample = std::numeric_limits<int16_t>::max();
  return static_cast<int16_t>((static_cast<int32_t>(percent) * kMaxSample) / 100);
}

constexpr std::array<float, ToneGenerator::kLutSize> BuildSineLut() {
  std::array<float, ToneGenerator::kLutSize> table{};
  for (size_t i = 0; i < ToneGenerator::kLutSize; ++i) {
    const float phase = (kTwoPi * static_cast<float>(i)) / static_cast<float>(ToneGenerator::kLutSize);
    table[i] = std::sin(phase);
  }
  return table;
}
}  // namespace

const std::array<float, ToneGenerator::kLutSize> ToneGenerator::kSineLut = BuildSineLut();

void ToneGenerator::Configure(const ToneGeneratorSettings& settings) {
  settings_ = settings;
  if (settings_.sample_rate_hz == 0) {
    settings_.sample_rate_hz = 1;  // Prevent division-by-zero when computing phase step.
  }
  amplitude_ = VolumePercentToAmplitude(settings_.volume_percent);
  pending_stop_ = false;
  fade_position_ = 0;
  state_ = State::kSilent;  // Start in silent state (volume 0)
  phase_ = 0.0f;
  UpdatePhaseStep();
  fade_in_samples_ = MillisecondsToSamples(settings_.fade_in_ms);
  fade_out_samples_ = MillisecondsToSamples(settings_.fade_out_ms);
}

void ToneGenerator::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_stop_ = false;

  // Handle different states to avoid clicks and support smooth transitions
  switch (state_) {
    case State::kSilent:
      // Normal start from silence - reset phase and begin fade-in
      fade_position_ = 0;
      phase_ = 0.0f;  // Reset phase to prevent click
      state_ = (fade_in_samples_ > 0) ? State::kFadeIn : State::kPlaying;
      break;

    case State::kFadeOut:
      // Reverse fade-out to fade-in (smooth transition)
      if (fade_out_samples_ > 0 && fade_in_samples_ > 0) {
        // Calculate current gain and mirror fade position
        const float current_gain = 1.0f - (static_cast<float>(fade_position_) / fade_out_samples_);
        fade_position_ = static_cast<size_t>(current_gain * fade_in_samples_);
      } else {
        fade_position_ = 0;
      }
      state_ = (fade_in_samples_ > 0) ? State::kFadeIn : State::kPlaying;
      break;

    case State::kFadeIn:
      // Already fading in - restart from beginning
      fade_position_ = 0;
      break;

    case State::kPlaying:
      // Already playing at full volume - do nothing (avoids phase reset and click)
      break;
  }
}

void ToneGenerator::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::kSilent || state_ == State::kFadeOut) {
    return;  // Already silent or fading to silent
  }
  pending_stop_ = true;
}

bool ToneGenerator::IsActive() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ != State::kSilent;  // Active when not silent
}

void ToneGenerator::SetFrequency(uint16_t frequency_hz) {
  if (frequency_hz == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.tone_frequency_hz = frequency_hz;
  UpdatePhaseStep();
}

uint16_t ToneGenerator::Frequency() const {
  return settings_.tone_frequency_hz;
}

void ToneGenerator::SetVolume(uint8_t percent) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.volume_percent = percent;
  amplitude_ = VolumePercentToAmplitude(percent);
}

uint8_t ToneGenerator::Volume() const {
  return settings_.volume_percent;
}

void ToneGenerator::SetFade(uint16_t fade_in_ms, uint16_t fade_out_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  settings_.fade_in_ms = fade_in_ms;
  settings_.fade_out_ms = fade_out_ms;
  fade_in_samples_ = MillisecondsToSamples(fade_in_ms);
  fade_out_samples_ = MillisecondsToSamples(fade_out_ms);
}

uint16_t ToneGenerator::FadeInMs() const {
  return settings_.fade_in_ms;
}

uint16_t ToneGenerator::FadeOutMs() const {
  return settings_.fade_out_ms;
}

void ToneGenerator::Fill(int16_t* stereo_buffer, size_t frames) {
  if (stereo_buffer == nullptr || frames == 0) {
    return;
  }

  // ===== LOCK 1: Snapshot state (~1µs) =====
  // Copy shared state to local variables to minimize mutex hold time
  State local_state;
  size_t local_fade_position;
  bool local_pending_stop;
  size_t local_fade_in_samples;
  size_t local_fade_out_samples;
  float local_phase;
  float local_phase_step;
  int16_t local_amplitude;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    local_state = state_;
    local_fade_position = fade_position_;
    local_pending_stop = pending_stop_;
    local_fade_in_samples = fade_in_samples_;
    local_fade_out_samples = fade_out_samples_;
    local_phase = phase_;
    local_phase_step = phase_step_;
    local_amplitude = amplitude_;
  }
  // Mutex released - Start()/Stop() can proceed immediately!

  // ===== Generate samples without lock (16ms) =====
  for (size_t idx = 0; idx < frames; ++idx) {
    float gain = 0.0f;
    switch (local_state) {
      case State::kSilent:
        gain = 0.0f;  // Generate at volume 0 (codec stays active)
        break;
      case State::kFadeIn: {
        // Check if Stop() was called during fade-in - reverse to fade-out immediately
        if (local_pending_stop) {
          if (local_fade_out_samples > 0 && local_fade_in_samples > 0) {
            // Calculate current gain and mirror to fade-out position
            const float current_gain = static_cast<float>(local_fade_position) / local_fade_in_samples;
            local_fade_position = static_cast<size_t>((1.0f - current_gain) * local_fade_out_samples);
          } else {
            local_fade_position = 0;
          }
          local_state = State::kFadeOut;
          local_pending_stop = false;
          gain = 0.0f;  // Will be recalculated in next iteration
        } else if (local_fade_in_samples == 0) {
          local_state = State::kPlaying;
          gain = 1.0f;
        } else {
          gain = static_cast<float>(local_fade_position) / static_cast<float>(local_fade_in_samples);
          if (gain >= 1.0f) {
            gain = 1.0f;
            local_state = State::kPlaying;
            local_fade_position = 0;
          } else {
            ++local_fade_position;
          }
        }
        break;
      }
      case State::kPlaying:
        gain = 1.0f;
        if (local_pending_stop) {
          local_state = State::kFadeOut;
          local_fade_position = 0;
          local_pending_stop = false;
        }
        break;
      case State::kFadeOut:
        if (local_fade_out_samples == 0) {
          gain = 0.0f;
          local_state = State::kSilent;  // Return to silent (not idle)
          local_pending_stop = false;
        } else {
          const float ratio = static_cast<float>(local_fade_position) / static_cast<float>(local_fade_out_samples);
          gain = 1.0f - ratio;
          if (gain <= 0.0f) {
            gain = 0.0f;
            local_state = State::kSilent;  // Return to silent (not idle)
            local_pending_stop = false;
          } else {
            ++local_fade_position;
          }
        }
        break;
    }

    const float waveform = SampleFromLut(local_phase);
    const float sample = waveform * static_cast<float>(local_amplitude) * gain;
    local_phase += local_phase_step;
    if (local_phase >= static_cast<float>(kLutSize)) {
      local_phase -= static_cast<float>(kLutSize);
    }

    const int16_t value = ConvertToSample(sample);
    const size_t base = idx * 2;
    stereo_buffer[base] = value;
    stereo_buffer[base + 1] = value;
  }

  // ===== LOCK 2: Update state (~1µs) =====
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = local_state;
    fade_position_ = local_fade_position;
    pending_stop_ = local_pending_stop;
    phase_ = local_phase;
  }
}

void ToneGenerator::UpdatePhaseStep() {
  phase_step_ = static_cast<float>(settings_.tone_frequency_hz) * static_cast<float>(kLutSize) /
                static_cast<float>(settings_.sample_rate_hz);
}

size_t ToneGenerator::MillisecondsToSamples(uint16_t duration_ms) const {
  const uint64_t samples = (static_cast<uint64_t>(settings_.sample_rate_hz) * duration_ms) / 1000ULL;
  return static_cast<size_t>((samples == 0ULL) ? 1ULL : samples);
}

int16_t ToneGenerator::ConvertToSample(float value) const {
  if (value > static_cast<float>(std::numeric_limits<int16_t>::max())) {
    return std::numeric_limits<int16_t>::max();
  }
  if (value < static_cast<float>(std::numeric_limits<int16_t>::min())) {
    return std::numeric_limits<int16_t>::min();
  }
  return static_cast<int16_t>(value);
}

float ToneGenerator::SampleFromLut(float phase_index) const {
  float wrapped = phase_index;
  if (wrapped >= static_cast<float>(kLutSize)) {
    wrapped = std::fmod(wrapped, static_cast<float>(kLutSize));
  } else if (wrapped < 0.0f) {
    wrapped = static_cast<float>(kLutSize) + std::fmod(wrapped, static_cast<float>(kLutSize));
  }

  const size_t index = static_cast<size_t>(wrapped);
  const size_t next_index = (index + 1U) % kLutSize;
  const float frac = wrapped - static_cast<float>(index);
  const float base = kSineLut[index];
  const float next = kSineLut[next_index];
  return base + (next - base) * frac;
}

}  // namespace audio
