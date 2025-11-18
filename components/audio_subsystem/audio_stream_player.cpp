#include "audio/audio_stream_player.hpp"

#include <algorithm>
#include <cstring>

namespace audio {

AudioStreamPlayer::AudioStreamPlayer() {
  Reset();
}

size_t AudioStreamPlayer::WriteALawSamples(const uint8_t* alaw_samples, size_t count) {
  const size_t free_space = GetFreeSpace();
  const size_t to_write = std::min(count, free_space);

  const size_t write_idx = write_index_.load(std::memory_order_acquire);

  for (size_t i = 0; i < to_write; ++i) {
    const size_t idx = (write_idx + i) % kRingBufferCapacity;
    ring_buffer_[idx] = ALawDecode(alaw_samples[i]);
  }

  write_index_.store((write_idx + to_write) % kRingBufferCapacity,
                     std::memory_order_release);

  // Start playing if we've reached minimum buffer threshold
  if (!playing_.load(std::memory_order_relaxed) && GetAvailableSamples() >= kMinBufferSamples) {
    playing_.store(true, std::memory_order_relaxed);
  }

  return to_write;
}

size_t AudioStreamPlayer::ReadStereoFrames(int16_t* stereo_buffer, size_t frames) {
  if (!playing_.load(std::memory_order_relaxed)) {
    // Not yet ready to play - return silence
    std::memset(stereo_buffer, 0, frames * 2 * sizeof(int16_t));
    return frames;
  }

  const size_t available = GetAvailableSamples();

  // Need frames/2 samples from 8kHz buffer to produce 'frames' @ 16kHz
  const size_t needed_8k_samples = (frames + 1) / 2;

  if (available < needed_8k_samples) {
    // Buffer underrun - return silence and mark as starved
    playing_.store(false, std::memory_order_relaxed);
    ++underrun_count_;
    std::memset(stereo_buffer, 0, frames * 2 * sizeof(int16_t));
    return frames;
  }

  const size_t read_idx = read_index_.load(std::memory_order_acquire);

  // Upsample 8kHz → 16kHz via linear interpolation
  for (size_t i = 0; i < frames; ++i) {
    int16_t sample;

    if (i % 2 == 0) {
      // Even frame: read new sample from 8kHz buffer
      const size_t idx = (read_idx + i / 2) % kRingBufferCapacity;
      sample = ring_buffer_[idx];
      last_sample_ = sample;
    } else {
      // Odd frame: interpolate between last and next sample
      const size_t idx_next = (read_idx + (i / 2) + 1) % kRingBufferCapacity;
      const int16_t next_sample = ring_buffer_[idx_next];
      sample = (last_sample_ + next_sample) / 2;
    }

    // Apply volume control (0-100%)
    if (volume_percent_ != 100) {
      // Use int32_t to avoid overflow during multiplication
      int32_t scaled_sample = (static_cast<int32_t>(sample) * volume_percent_) / 100;
      // Clamp to int16_t range to prevent overflow
      sample = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, scaled_sample)));
    }

    // Duplicate mono → stereo
    stereo_buffer[i * 2] = sample;      // Left
    stereo_buffer[i * 2 + 1] = sample;  // Right
  }

  read_index_.store((read_idx + needed_8k_samples) % kRingBufferCapacity,
                    std::memory_order_release);

  return frames;
}

size_t AudioStreamPlayer::GetAvailableSamples() const {
  const size_t write_idx = write_index_.load(std::memory_order_acquire);
  const size_t read_idx = read_index_.load(std::memory_order_acquire);

  if (write_idx >= read_idx) {
    return write_idx - read_idx;
  } else {
    return kRingBufferCapacity - read_idx + write_idx;
  }
}

size_t AudioStreamPlayer::GetFreeSpace() const {
  return kRingBufferCapacity - GetAvailableSamples() - 1;  // Keep 1 slot empty
}

bool AudioStreamPlayer::IsReady() const {
  return GetAvailableSamples() >= kMinBufferSamples;
}

void AudioStreamPlayer::Reset() {
  write_index_.store(0, std::memory_order_release);
  read_index_.store(0, std::memory_order_release);
  playing_.store(false, std::memory_order_release);
  last_sample_ = 0;
  underrun_count_ = 0;
  ring_buffer_.fill(0);
}

void AudioStreamPlayer::SetVolume(uint8_t volume_percent) {
  // Clamp to 0-100 range
  volume_percent_ = (volume_percent > 100) ? 100 : volume_percent;
}

}  // namespace audio
