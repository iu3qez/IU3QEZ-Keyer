#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#include "audio/sidetone_service.hpp"
#include "audio/tone_generator.hpp"
#include "support/fake_esp_idf.hpp"

namespace {

using audio::CodecDriver;
using audio::CodecDriverConfig;
using audio::SidetoneConfig;
using audio::SidetoneService;

class FakeCodecDriver : public CodecDriver {
 public:
  esp_err_t Initialize(const CodecDriverConfig& config) override {
    ++initialize_calls;
    last_config = config;
    return initialize_result;
  }

  esp_err_t SetMute(bool mute) override {
    mute_calls.push_back(mute);
    return set_mute_result;
  }

  esp_err_t SetVolume(uint8_t volume_percent) override {
    volume_calls.push_back(volume_percent);
    return set_volume_result;
  }

  esp_err_t Write(const int16_t* data, size_t byte_count) override {
    WriteRecord record;
    record.bytes = byte_count;
    if (data != nullptr) {
      const size_t samples = byte_count / sizeof(int16_t);
      record.samples.assign(data, data + samples);
    }
    write_records.push_back(std::move(record));
    return write_result;
  }

  void Shutdown() override { ++shutdown_calls; }

  struct WriteRecord {
    size_t bytes = 0;
    std::vector<int16_t> samples;
  };

  int initialize_calls = 0;
  int shutdown_calls = 0;
  std::vector<bool> mute_calls;
  std::vector<uint8_t> volume_calls;
  std::vector<WriteRecord> write_records;
  CodecDriverConfig last_config{};

  esp_err_t initialize_result = ESP_OK;
  esp_err_t set_mute_result = ESP_OK;
  esp_err_t set_volume_result = ESP_OK;
  esp_err_t write_result = ESP_OK;
};

class SidetoneServiceTest : public ::testing::Test {
 protected:
  void SetUp() override { fake_esp_idf_reset(); }

  static SidetoneConfig DefaultConfig() {
    SidetoneConfig config;
    config.i2c_port = 0;
    config.i2c_sda = static_cast<gpio_num_t>(1);
    config.i2c_scl = static_cast<gpio_num_t>(2);
    config.i2s_port = 0;
    config.i2s_bclk = static_cast<gpio_num_t>(3);
    config.i2s_lrck = static_cast<gpio_num_t>(4);
    config.i2s_mclk = static_cast<gpio_num_t>(5);
    config.i2s_dout = static_cast<gpio_num_t>(6);
    config.tone_settings.volume_percent = 50;
    config.tone_settings.tone_frequency_hz = 600;
    return config;
  }
};

TEST_F(SidetoneServiceTest, InitializeAndStartInteractWithCodecDriver) {
  SidetoneService service;
  auto driver = std::make_unique<FakeCodecDriver>();
  FakeCodecDriver* driver_ptr = driver.get();
  service.SetCodecDriverForTesting(std::move(driver));

  SidetoneConfig config = DefaultConfig();
  ASSERT_EQ(ESP_OK, service.Initialize(config));

  EXPECT_EQ(2, driver_ptr->shutdown_calls);  // Deinitialize and ConfigureCodec both call Shutdown
  EXPECT_EQ(1, driver_ptr->initialize_calls);
  EXPECT_EQ(config.sample_rate_hz, driver_ptr->last_config.sample_rate_hz);
  EXPECT_EQ(config.i2c_port, driver_ptr->last_config.i2c_port);
  EXPECT_EQ(config.i2s_port, driver_ptr->last_config.i2s_port);
  EXPECT_EQ(config.tone_settings.volume_percent, driver_ptr->last_config.initial_volume_percent);
  ASSERT_NE(nullptr, driver_ptr->last_config.i2s_tx_handle);

  ASSERT_EQ(ESP_OK, service.Start());
  ASSERT_FALSE(driver_ptr->mute_calls.empty());
  EXPECT_FALSE(driver_ptr->mute_calls.front());

  service.SetVolume(73);
  ASSERT_FALSE(driver_ptr->volume_calls.empty());
  EXPECT_EQ(73, driver_ptr->volume_calls.back());

  service.Stop();
  // Codec mute removed from Stop(): fade out already brings volume to 0, hardware mute causes click
  // Only unmute is called on Start(), no mute on Stop()
  ASSERT_EQ(driver_ptr->mute_calls.size(), 1u);
  EXPECT_FALSE(driver_ptr->mute_calls.back());  // Only unmute from Start()
}

TEST_F(SidetoneServiceTest, PumpAudioOnceProducesStereoFrames) {
  SidetoneService service;
  auto driver = std::make_unique<FakeCodecDriver>();
  FakeCodecDriver* driver_ptr = driver.get();
  service.SetCodecDriverForTesting(std::move(driver));

  SidetoneConfig config = DefaultConfig();
  ASSERT_EQ(ESP_OK, service.Initialize(config));
  ASSERT_EQ(ESP_OK, service.Start());
  service.SetFade(4, 6);

  EXPECT_EQ(ESP_OK, service.PumpAudioOnceForTest());
  ASSERT_EQ(1u, driver_ptr->write_records.size());
  const auto& first = driver_ptr->write_records.front();
  EXPECT_EQ(audio::SidetoneService::kFramesPerChunk * audio::SidetoneService::kCodecChannelCount,
            first.samples.size());
  EXPECT_EQ(first.bytes, first.samples.size() * sizeof(int16_t));
  EXPECT_TRUE(std::any_of(first.samples.begin(), first.samples.end(), [](int16_t value) { return value != 0; }));

  EXPECT_EQ(ESP_OK, service.PumpAudioOnceForTest());
  EXPECT_EQ(2u, driver_ptr->write_records.size());
}

TEST_F(SidetoneServiceTest, PumpAudioOncePropagatesDriverFailure) {
  SidetoneService service;
  auto driver = std::make_unique<FakeCodecDriver>();
  FakeCodecDriver* driver_ptr = driver.get();
  driver_ptr->write_result = ESP_FAIL;
  service.SetCodecDriverForTesting(std::move(driver));

  SidetoneConfig config = DefaultConfig();
  ASSERT_EQ(ESP_OK, service.Initialize(config));
  ASSERT_EQ(ESP_OK, service.Start());

  EXPECT_EQ(ESP_FAIL, service.PumpAudioOnceForTest());
  ASSERT_EQ(1u, driver_ptr->write_records.size());
}

TEST(ToneGeneratorTest, GeneratesThreeBurstsWithPreciseTiming) {
  audio::ToneGenerator generator;
  audio::ToneGeneratorSettings settings{};
  settings.sample_rate_hz = 48000;
  settings.tone_frequency_hz = 650;
  settings.volume_percent = 70;
  settings.fade_in_ms = 8;
  settings.fade_out_ms = 8;
  generator.Configure(settings);

  const size_t frames_per_tone = static_cast<size_t>(settings.sample_rate_hz * 144 / 1000);
  const size_t fade_samples = static_cast<size_t>(settings.sample_rate_hz * settings.fade_out_ms / 1000);
  const size_t frames_before_stop = frames_per_tone - fade_samples;
  const size_t gap_frames = static_cast<size_t>(settings.sample_rate_hz * 44 / 1000);

  auto append_frames = [&](size_t frames, std::vector<int16_t>& dest) {
    const size_t kChunkFrames = 256;
    while (frames > 0) {
      const size_t chunk = std::min(frames, kChunkFrames);
      std::vector<int16_t> raw(chunk * 2);
      generator.Fill(raw.data(), chunk);
      for (size_t i = 0; i < chunk; ++i) {
        dest.push_back(raw[i * 2]);
      }
      frames -= chunk;
    }
  };

  std::vector<int16_t> waveform;
  std::vector<std::pair<size_t, size_t>> tone_segments;
  waveform.reserve(3 * frames_per_tone + 2 * gap_frames);

  for (int tone = 0; tone < 3; ++tone) {
    generator.Start();
    const size_t start_index = waveform.size();

    append_frames(frames_before_stop, waveform);
    generator.Stop();
    size_t fade_frames_accumulated = 0;
    while (generator.IsActive()) {
      const size_t before = waveform.size();
      append_frames(32, waveform);
      fade_frames_accumulated += waveform.size() - before;
      ASSERT_LT(fade_frames_accumulated, fade_samples * 2);
    }

    const size_t tone_length = waveform.size() - start_index;
    tone_segments.emplace_back(start_index, tone_length);
    EXPECT_NEAR(static_cast<double>(tone_length), static_cast<double>(frames_per_tone), 64.0);

    if (tone < 2) {
      append_frames(gap_frames, waveform);
    }
  }

  const size_t expected_total = 3 * frames_per_tone + 2 * gap_frames;
  ASSERT_NEAR(static_cast<double>(expected_total), static_cast<double>(waveform.size()), 192.0);
  ASSERT_EQ(3u, tone_segments.size());

  const int32_t expected_amplitude =
      static_cast<int32_t>(settings.volume_percent * std::numeric_limits<int16_t>::max() / 100);

  for (size_t i = 0; i < tone_segments.size(); ++i) {
    const size_t start = tone_segments[i].first;
    const size_t length = tone_segments[i].second;

    int32_t max_abs = 0;
    for (size_t idx = 0; idx < length; ++idx) {
      const int32_t sample = std::abs(static_cast<int32_t>(waveform[start + idx]));
      if (sample > max_abs) {
        max_abs = sample;
      }
    }
    EXPECT_GE(max_abs, static_cast<int32_t>(expected_amplitude * 9 / 10));

    const int32_t fade_in_sample = std::abs(static_cast<int32_t>(waveform[start]));
    const int32_t fade_out_sample = std::abs(static_cast<int32_t>(waveform[start + length - 1]));
    EXPECT_LE(fade_in_sample, static_cast<int32_t>(expected_amplitude / 5));
    EXPECT_LE(fade_out_sample, static_cast<int32_t>(expected_amplitude / 5));

    if (i < tone_segments.size() - 1) {
      const size_t gap_start = tone_segments[i].first + tone_segments[i].second;
      for (size_t idx = 0; idx < gap_frames; ++idx) {
        EXPECT_NEAR(0, waveform[gap_start + idx], 2);
      }
    }
  }
}

}  // namespace
