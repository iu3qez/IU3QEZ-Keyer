#include <gtest/gtest.h>
#include "audio/audio_stream_player.hpp"
#include <algorithm>

using namespace audio;

class AudioStreamPlayerTest : public ::testing::Test {
 protected:
  AudioStreamPlayer player_;
};

TEST_F(AudioStreamPlayerTest, InitiallyEmpty) {
  EXPECT_EQ(0, player_.GetAvailableSamples());
  EXPECT_FALSE(player_.IsPlaying());
}

TEST_F(AudioStreamPlayerTest, InitiallyHasCapacity) {
  EXPECT_GT(player_.GetFreeSpace(), 0);
}

TEST_F(AudioStreamPlayerTest, WriteALawSamples) {
  // A-Law silence byte = 0xD5 (decodes to ~0)
  uint8_t alaw_samples[10] = {0xD5, 0xD5, 0xD5, 0xD5, 0xD5,
                               0xD5, 0xD5, 0xD5, 0xD5, 0xD5};

  const size_t written = player_.WriteALawSamples(alaw_samples, 10);

  EXPECT_EQ(10, written);
  EXPECT_EQ(10, player_.GetAvailableSamples());
  EXPECT_FALSE(player_.IsPlaying());  // Below kMinBufferSamples (320)
}

TEST_F(AudioStreamPlayerTest, StartsPlayingAfterMinBuffer) {
  // Fill buffer to kMinBufferSamples (320 samples)
  uint8_t alaw_samples[320];
  std::fill_n(alaw_samples, 320, 0xD5);  // A-Law silence

  const size_t written = player_.WriteALawSamples(alaw_samples, 320);

  EXPECT_EQ(320, written);
  EXPECT_TRUE(player_.IsPlaying());
}

TEST_F(AudioStreamPlayerTest, ReadStereoFramesBeforeReady) {
  int16_t stereo_buffer[512];  // 256 stereo frames @ 16kHz

  const size_t read = player_.ReadStereoFrames(stereo_buffer, 256);

  EXPECT_EQ(256, read);
  // Should return silence (zeros)
  for (size_t i = 0; i < 512; ++i) {
    EXPECT_EQ(0, stereo_buffer[i]);
  }
}

TEST_F(AudioStreamPlayerTest, ReadStereoFramesAfterWrite) {
  // Write 320 A-Law samples (minimum to start playing)
  uint8_t alaw_samples[320];
  // A-Law 0x80 = positive max (~5504)
  std::fill_n(alaw_samples, 320, 0x80);

  player_.WriteALawSamples(alaw_samples, 320);
  ASSERT_TRUE(player_.IsPlaying());

  // Read 160 stereo frames @ 16kHz (requires 80 samples @ 8kHz)
  int16_t stereo_buffer[320];  // 160 frames * 2 channels
  const size_t read = player_.ReadStereoFrames(stereo_buffer, 160);

  EXPECT_EQ(160, read);
  EXPECT_EQ(320 - 80, player_.GetAvailableSamples());  // 240 left

  // Verify non-zero output (decoded A-Law)
  EXPECT_NE(0, stereo_buffer[0]);
  EXPECT_EQ(stereo_buffer[0], stereo_buffer[1]);  // Left == Right
}

TEST_F(AudioStreamPlayerTest, UnderrunStopsPlayback) {
  // Write 320 samples, then read more than available
  uint8_t alaw_samples[320];
  std::fill_n(alaw_samples, 320, 0x80);
  player_.WriteALawSamples(alaw_samples, 320);

  ASSERT_TRUE(player_.IsPlaying());

  // Read 320 frames @ 16kHz (requires 160 samples @ 8kHz)
  int16_t stereo_buffer[640];
  player_.ReadStereoFrames(stereo_buffer, 320);

  // Second read should cause underrun (160 samples left, need 160)
  player_.ReadStereoFrames(stereo_buffer, 320);

  EXPECT_FALSE(player_.IsPlaying());
  EXPECT_EQ(1, player_.GetUnderrunCount());
}
