#include "keying/paddle_engine.hpp"

#include "gtest/gtest.h"

#include <vector>

#include "hal/paddle_hal.hpp"

namespace {

struct CallbackRecorder {
  struct ElementEvent {
    keying::PaddleElement element;
    bool started;
    int64_t timestamp_us;
  };
  struct KeyEvent {
    bool active;
    int64_t timestamp_us;
  };

  static void OnStarted(keying::PaddleElement element, int64_t timestamp_us,
                        void* context) {
    auto* self = static_cast<CallbackRecorder*>(context);
    self->elements.push_back(ElementEvent{element, true, timestamp_us});
  }

  static void OnFinished(keying::PaddleElement element, int64_t timestamp_us,
                         void* context) {
    auto* self = static_cast<CallbackRecorder*>(context);
    self->elements.push_back(ElementEvent{element, false, timestamp_us});
  }

  static void OnKeyChanged(bool active, int64_t timestamp_us, void* context) {
    auto* self = static_cast<CallbackRecorder*>(context);
    self->key_states.push_back(KeyEvent{active, timestamp_us});
  }

  std::vector<ElementEvent> elements;
  std::vector<KeyEvent> key_states;
};

int64_t DitDurationUs(uint32_t wpm) {
  const double seconds = 1.2 / static_cast<double>(wpm);
  return static_cast<int64_t>(seconds * 1'000'000.0);
}

TEST(PaddleEngineTest, ImmediateDitSchedulesElement) {
  keying::PaddleEngine engine;
  keying::PaddleEngineConfig config{};
  CallbackRecorder recorder;
  keying::PaddleEngineCallbacks callbacks{
      .on_element_started = CallbackRecorder::OnStarted,
      .on_element_finished = CallbackRecorder::OnFinished,
      .on_key_state_changed = CallbackRecorder::OnKeyChanged,
      .context = &recorder,
  };
  ASSERT_TRUE(engine.Initialize(config, callbacks));

  const int64_t press_time = 1'000;
  engine.OnPaddleEvent({.line = hal::PaddleLine::kDit, .active = true, .timestamp_us = press_time});
  engine.Tick(press_time);

  ASSERT_EQ(1u, recorder.elements.size());
  EXPECT_TRUE(recorder.elements[0].started);
  EXPECT_EQ(keying::PaddleElement::kDit, recorder.elements[0].element);
  EXPECT_EQ(press_time, recorder.elements[0].timestamp_us);
}

TEST(PaddleEngineTest, MemoryWindowCapturesSqueezeWithinRange) {
  keying::PaddleEngine engine;
  keying::PaddleEngineConfig config{};
  config.speed_wpm = 20;
  config.mem_block_start_pct = 60.0f;
  config.mem_block_end_pct = 99.0f;
  CallbackRecorder recorder;
  keying::PaddleEngineCallbacks callbacks{
      .on_element_started = CallbackRecorder::OnStarted,
      .on_element_finished = CallbackRecorder::OnFinished,
      .on_key_state_changed = CallbackRecorder::OnKeyChanged,
      .context = &recorder,
  };
  ASSERT_TRUE(engine.Initialize(config, callbacks));

  const int64_t dit_duration = DitDurationUs(config.speed_wpm);
  engine.OnPaddleEvent({.line = hal::PaddleLine::kDit, .active = true, .timestamp_us = 0});
  engine.Tick(0);

  const int64_t squeeze_time = (dit_duration * 60) / 100;
  engine.OnPaddleEvent({.line = hal::PaddleLine::kDah, .active = true, .timestamp_us = squeeze_time});
  engine.Tick(squeeze_time);

  engine.Tick(dit_duration);
  engine.Tick(dit_duration + DitDurationUs(config.speed_wpm));

  ASSERT_GE(recorder.elements.size(), 2u);
  EXPECT_EQ(keying::PaddleElement::kDah, recorder.elements.back().element);
  EXPECT_TRUE(recorder.elements.back().started);
}

}  // namespace
