#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__has_include)
#if __has_include("esp_attr.h")
extern "C" {
#include "esp_attr.h"
#include "freertos/portmacro.h"
}
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef portMUX_TYPE
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) (void)(mux)
#define portEXIT_CRITICAL(mux) (void)(mux)
#define portENTER_CRITICAL_ISR(mux) (void)(mux)
#define portEXIT_CRITICAL_ISR(mux) (void)(mux)
#endif
#endif
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

namespace timeline {

enum class EventType : uint8_t {
  kPaddleEdge = 0,
  kRemoteEvent = 1,
  kDiagnostics = 2,
  kAudio = 3,
  kKeying = 4,
  kMemoryWindow = 5,   // arg0: 0=dit/1=dah, arg1: 0=closed/1=open
  kLatch = 6,          // arg0: unused, arg1: 0=released/1=active
  kSqueeze = 7,        // arg0: unused, arg1: 1=detected
  kGapMarker = 8,      // arg0: 0=element/1=char/2=word, arg1: unused
  kDecodedChar = 9,    // arg0: char_code (ASCII), arg1: unused
};

struct TimelineEvent {
  int64_t timestamp_us = 0;
  EventType type = EventType::kDiagnostics;
  uint32_t arg0 = 0;
  uint32_t arg1 = 0;
};

template <size_t Capacity>
class EventLogger {
  static_assert(Capacity > 0, "EventLogger capacity must be greater than zero");

 public:
  constexpr EventLogger() : spinlock_(portMUX_INITIALIZER_UNLOCKED) {}

  // Thread-safe push for task context. Uses critical section but may allocate literals.
  void push(const TimelineEvent& event) noexcept {
    portENTER_CRITICAL(&spinlock_);
    push_unsafe(event);
    portEXIT_CRITICAL(&spinlock_);
  }

  // ISR-safe push. Must be called from ISR context only. Uses ISR-specific critical section.
  // NOTE: Capacity template parameter must be a power of 2 for optimal code generation,
  // or use push() from task context via deferred queue to avoid IRAM literal issues.
  void IRAM_ATTR push_isr(const TimelineEvent& event) noexcept {
    portENTER_CRITICAL_ISR(&spinlock_);
    push_unsafe(event);
    portEXIT_CRITICAL_ISR(&spinlock_);
  }

  [[nodiscard]] size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity; }
  [[nodiscard]] size_t dropped_count() const noexcept { return dropped_count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

  void clear() noexcept {
    head_ = 0;
    count_ = 0;
    dropped_count_ = 0;
  }

  template <typename Visitor>
  void for_each(Visitor&& visitor) const {
    const size_t valid = count_;
    if (valid == 0) {
      return;
    }
    const size_t start = (valid == Capacity) ? head_ : 0;
    for (size_t i = 0; i < valid; ++i) {
      size_t index = start + i;
      if (index >= Capacity) {  // Avoid modulo to prevent literal relocation.
        index -= Capacity;
      }
      visitor(buffer_[index]);
    }
  }

  [[nodiscard]] TimelineEvent latest() const noexcept {
    if (count_ == 0) {
      return TimelineEvent{};
    }
    const size_t index = (head_ > 0) ? (head_ - 1) : (Capacity - 1);  // Avoid modulo to prevent literal relocation.
    return buffer_[index];
  }

 private:
  // Unsafe push without locking. Must be called within a critical section.
  void push_unsafe(const TimelineEvent& event) noexcept {
    buffer_[head_] = event;
    const size_t next_head = head_ + 1;
    head_ = (next_head < Capacity) ? next_head : 0;  // Avoid modulo to prevent literal relocation.
    if (count_ < Capacity) {
      ++count_;
    } else {
      ++dropped_count_;
    }
  }

  TimelineEvent buffer_[Capacity] = {};
  size_t head_ = 0;
  size_t count_ = 0;
  size_t dropped_count_ = 0;
  mutable portMUX_TYPE spinlock_;  // Spinlock for ISR-safe access. Mutable to allow locking in const methods if needed.
};

}  // namespace timeline
