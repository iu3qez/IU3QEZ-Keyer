#include "timeline/timeline_event_emitter.hpp"

#include <cinttypes>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
// Host testing fallback
#include <cstdio>
#define ESP_LOGD(tag, format, ...) printf("[DEBUG][%s] " format "\n", tag, ##__VA_ARGS__)
#endif

namespace timeline {

namespace {
constexpr const char* TAG = "TimelineEmitter";
}  // namespace

// Constructor: Store injected EventLogger pointer
TimelineEventEmitter::TimelineEventEmitter(EventLogger<1024>* event_logger)
    : event_logger_(event_logger) {
  // No validation - caller must ensure event_logger is valid
}

// Get hooks struct for dependency injection
TimelineHooks TimelineEventEmitter::GetHooks() {
  return TimelineHooks{
      .OnMemoryWindowChanged = &HandleMemoryWindowChanged,
      .OnLatchStateChanged = &HandleLatchStateChanged,
      .OnSqueezeDetected = &HandleSqueezeDetected,
      .context = this  // Context pointer to this emitter instance
  };
}

// Hook handler: Memory window state changed
void TimelineEventEmitter::HandleMemoryWindowChanged(bool is_dah, bool opened,
                                                      int64_t ts, void* ctx) {
  auto* emitter = static_cast<TimelineEventEmitter*>(ctx);

  // Create event
  TimelineEvent evt{
      .timestamp_us = ts,
      .type = EventType::kMemoryWindow,
      .arg0 = is_dah ? 1U : 0U,   // 0=dit, 1=dah
      .arg1 = opened ? 1U : 0U    // 0=closed, 1=open
  };

  // Push to event logger
  emitter->event_logger_->push(evt);
}

// Hook handler: Latch state changed
void TimelineEventEmitter::HandleLatchStateChanged(bool active, int64_t ts,
                                                    void* ctx) {
  auto* emitter = static_cast<TimelineEventEmitter*>(ctx);

  // Create event
  TimelineEvent evt{
      .timestamp_us = ts,
      .type = EventType::kLatch,
      .arg0 = 0U,             // Unused (reserved)
      .arg1 = active ? 1U : 0U  // 0=released, 1=active
  };

  // Push to event logger
  emitter->event_logger_->push(evt);
}

// Hook handler: Squeeze detected
void TimelineEventEmitter::HandleSqueezeDetected(int64_t ts, void* ctx) {
  auto* emitter = static_cast<TimelineEventEmitter*>(ctx);

  // Create event
  TimelineEvent evt{
      .timestamp_us = ts,
      .type = EventType::kSqueeze,
      .arg0 = 0U,  // Unused (reserved)
      .arg1 = 1U   // Always 1 (detected)
  };

  // Push to event logger
  emitter->event_logger_->push(evt);
}

}  // namespace timeline
