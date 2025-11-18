#pragma once

/**
 * @file audio_stream_player.hpp
 * @brief Ring buffer for remote audio stream playback @ 8kHz
 *
 * ARCHITECTURE:
 * - Receives A-Law compressed mono samples from RemoteCwClient
 * - Decodes to 16-bit PCM using alaw_codec.hpp lookup table
 * - Stores in ring buffer (capacity: 160ms @ 8kHz = 1280 samples)
 * - Upsamples 8kHz→16kHz via linear interpolation for SidetoneService
 * - Thread-safe: Write from network task, Read from audio task
 *
 * LATENCY MANAGEMENT:
 * - Min buffer: 40ms (320 samples @ 8kHz) before starting playback
 * - Target buffer: 80ms (640 samples) for jitter absorption
 * - Max buffer: 160ms (1280 samples) - discard if overflow
 *
 * SAMPLE RATE CONVERSION:
 * - Input: 8000 Hz mono (from network)
 * - Output: 16000 Hz stereo (for ES8311 codec)
 * - Method: Linear interpolation (simple 2x upsampling)
 * - Quality: Sufficient for CW sidetone (narrow bandwidth)
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "audio/alaw_codec.hpp"

namespace audio {

class AudioStreamPlayer {
 public:
  AudioStreamPlayer();

  /**
   * @brief Write A-Law compressed samples to ring buffer.
   * @param alaw_samples A-Law encoded bytes (mono @ 8kHz).
   * @param count Number of samples to write.
   * @return Number of samples actually written (may be < count if buffer full).
   */
  size_t WriteALawSamples(const uint8_t* alaw_samples, size_t count);

  /**
   * @brief Read upsampled stereo PCM samples for codec output.
   * @param stereo_buffer Output buffer (16-bit stereo interleaved).
   * @param frames Number of stereo frames requested (@ 16kHz).
   * @return Number of frames actually filled.
   *
   * Internally upsamples 8kHz mono → 16kHz stereo via linear interpolation.
   */
  size_t ReadStereoFrames(int16_t* stereo_buffer, size_t frames);

  /**
   * @brief Get number of 8kHz mono samples available in buffer.
   */
  size_t GetAvailableSamples() const;

  /**
   * @brief Get free space in buffer (8kHz mono samples).
   */
  size_t GetFreeSpace() const;

  /**
   * @brief Check if player has enough buffered data to start playback.
   */
  bool IsReady() const;

  /**
   * @brief Check if player is actively playing (not starved).
   */
  bool IsPlaying() const { return playing_.load(std::memory_order_relaxed); }

  /**
   * @brief Reset buffer and playback state.
   */
  void Reset();

  /**
   * @brief Get buffer underrun count (diagnostic).
   */
  uint32_t GetUnderrunCount() const { return underrun_count_; }

  /**
   * @brief Set volume for remote audio stream (0-100%).
   * @param volume_percent Volume level (0 = mute, 100 = maximum).
   */
  void SetVolume(uint8_t volume_percent);

  /**
   * @brief Get current volume setting (0-100%).
   */
  uint8_t GetVolume() const { return volume_percent_; }

  static constexpr size_t kRingBufferCapacity = 1280;  // 160ms @ 8kHz
  static constexpr size_t kMinBufferSamples = 320;     // 40ms @ 8kHz
  static constexpr size_t kTargetBufferSamples = 640;  // 80ms @ 8kHz

 private:
  // Ring buffer: stores decoded 16-bit PCM @ 8kHz mono
  std::array<int16_t, kRingBufferCapacity> ring_buffer_{};
  std::atomic<size_t> write_index_{0};
  std::atomic<size_t> read_index_{0};
  std::atomic<bool> playing_{false};

  // Sample rate conversion state
  int16_t last_sample_{0};  // For linear interpolation

  uint32_t underrun_count_{0};
  uint8_t volume_percent_{100};  // Volume control (0-100%, default 100)
};

}  // namespace audio
