#pragma once

#include "timeline/event_logger.hpp"
#include "timeline/timeline_hooks.hpp"

/**
 * @file timeline_event_emitter.hpp
 * @brief Timeline event emitter with hook-based architecture
 *
 * OVERVIEW
 * ========
 * TimelineEventEmitter implements the hook callback pattern for timeline event
 * emission. It acts as the bridge between low-level keying components (like
 * PaddleEngine) and the timeline EventLogger, enabling instrumentation without
 * creating direct component dependencies.
 *
 * ARCHITECTURE
 * ============
 * The emitter implements static handler methods that conform to the function
 * pointer signatures defined in TimelineHooks. These static methods:
 * 1. Receive a void* context pointer (cast to TimelineEventEmitter*)
 * 2. Extract event parameters from hook arguments
 * 3. Create TimelineEvent structs with appropriate EventType
 * 4. Push events to the injected EventLogger instance
 *
 * DEPENDENCY INJECTION
 * ====================
 * TimelineEventEmitter does NOT own the EventLogger. The logger is injected
 * via constructor and owned by KeyingSubsystem. This allows:
 * - Shared event logger across multiple emitters (if needed)
 * - Testability (mock EventLogger for unit tests)
 * - Clear ownership semantics (KeyingSubsystem owns timeline data)
 *
 * USAGE PATTERN
 * =============
 * @code
 * // In ApplicationController initialization:
 * auto& timeline = keying_subsystem->GetTimeline();
 * auto emitter = std::make_unique<TimelineEventEmitter>(&timeline);
 * keying_subsystem->SetTimelineEmitter(emitter.get());
 *
 * // In KeyingSubsystem::SetTimelineEmitter():
 * TimelineHooks hooks = emitter->GetHooks();
 * paddle_callbacks_.timeline_hooks = hooks;
 *
 * // In PaddleEngine (at instrumentation point):
 * if (callbacks_.timeline_hooks.OnMemoryWindowChanged != nullptr) {
 *   callbacks_.timeline_hooks.OnMemoryWindowChanged(
 *     false, true, hal::HighPrecisionClock::NowMicros(),
 *     callbacks_.timeline_hooks.context
 *   );
 * }
 * @endcode
 *
 * THREAD SAFETY
 * =============
 * TimelineEventEmitter is designed for single-threaded usage in the keying
 * subsystem context. Thread safety is delegated to EventLogger, which uses
 * critical sections for push() operations. Hook handlers may be called from:
 * - Task context (normal keying flow)
 * - ISR context (paddle edge interrupts)
 *
 * EventLogger::push() is thread-safe and ISR-safe, so hook handlers can be
 * called from any context.
 *
 * @see timeline_hooks.hpp for hook definitions and documentation
 * @see event_logger.hpp for EventLogger implementation
 * @see docs/TIMELINE_HOOKS.md for integration guide
 */

namespace timeline {

/**
 * @brief Timeline event emitter with hook callback implementations
 *
 * This class implements the timeline hook pattern by providing static callback
 * methods that conform to TimelineHooks function pointer signatures. It acts
 * as the glue between keying components and the timeline event logger.
 *
 * **Design Pattern:** Static handlers with context pointer (C-style callbacks)
 *
 * **Ownership:** Does NOT own EventLogger (injected dependency)
 *
 * **Lifecycle:** Created by ApplicationController during initialization,
 * lives for the entire application lifetime.
 */
class TimelineEventEmitter {
 public:
  /**
   * @brief Construct event emitter with injected EventLogger
   *
   * The event logger is NOT owned by this emitter. The caller (typically
   * ApplicationController) must ensure the logger outlives this emitter.
   *
   * @param event_logger Pointer to EventLogger instance (must not be nullptr)
   *
   * @note No validation is performed on event_logger pointer. Caller must
   *       ensure it points to a valid EventLogger instance.
   */
  explicit TimelineEventEmitter(EventLogger<1024>* event_logger);

  /**
   * @brief Hook callback for memory window state changes
   *
   * This static method is called via TimelineHooks::OnMemoryWindowChanged when
   * the iambic memory window opens or closes. It creates a kMemoryWindow event
   * and pushes it to the EventLogger.
   *
   * **Event encoding:**
   * - type: EventType::kMemoryWindow
   * - arg0: 0 = dit window, 1 = dah window
   * - arg1: 0 = closed, 1 = open
   * - timestamp_us: microsecond timestamp from hook caller
   *
   * **Called from:** PaddleEngine::UpdateMemoryState() (approximate line 180-190)
   *
   * @param is_dah true if dah memory window, false if dit memory window
   * @param opened true if window opened, false if closed/expired
   * @param ts microsecond timestamp when state changed
   * @param ctx opaque context pointer (must be TimelineEventEmitter*)
   */
  static void HandleMemoryWindowChanged(bool is_dah, bool opened, int64_t ts, void* ctx);

  /**
   * @brief Hook callback for latch state changes
   *
   * This static method is called via TimelineHooks::OnLatchStateChanged when
   * the latch feature engages or releases. It creates a kLatch event and
   * pushes it to the EventLogger.
   *
   * **Event encoding:**
   * - type: EventType::kLatch
   * - arg0: 0 (unused, reserved for future use)
   * - arg1: 0 = released, 1 = active (latch holding)
   * - timestamp_us: microsecond timestamp from hook caller
   *
   * **Called from:** PaddleEngine::UpdateLatchState() (approximate line 250-260)
   *
   * @param active true if latch engaged (holding), false if released
   * @param ts microsecond timestamp when latch state changed
   * @param ctx opaque context pointer (must be TimelineEventEmitter*)
   */
  static void HandleLatchStateChanged(bool active, int64_t ts, void* ctx);

  /**
   * @brief Hook callback for squeeze detection
   *
   * This static method is called via TimelineHooks::OnSqueezeDetected when
   * both paddles are pressed simultaneously. It creates a kSqueeze event and
   * pushes it to the EventLogger.
   *
   * **Event encoding:**
   * - type: EventType::kSqueeze
   * - arg0: 0 (unused, reserved for future use)
   * - arg1: 1 (always 1 to indicate detected)
   * - timestamp_us: microsecond timestamp from hook caller
   *
   * **Called from:** PaddleEngine::HandlePaddlePress() (approximate line 140-150)
   *
   * @param ts microsecond timestamp when squeeze detected
   * @param ctx opaque context pointer (must be TimelineEventEmitter*)
   */
  static void HandleSqueezeDetected(int64_t ts, void* ctx);

  /**
   * @brief Get TimelineHooks struct for dependency injection
   *
   * Returns a TimelineHooks struct with function pointers set to this emitter's
   * static handler methods and context pointer set to this instance.
   *
   * **Usage pattern:**
   * @code
   * TimelineHooks hooks = emitter->GetHooks();
   * paddle_callbacks_.timeline_hooks = hooks;
   * @endcode
   *
   * The returned hooks struct can be copied and stored in PaddleEngineCallbacks
   * or similar callback structures.
   *
   * @return TimelineHooks struct configured for this emitter instance
   */
  TimelineHooks GetHooks();

 private:
  /**
   * @brief Injected EventLogger instance (NOT owned)
   *
   * This pointer is set during construction and must remain valid for the
   * lifetime of this emitter. The EventLogger is typically owned by
   * KeyingSubsystem and shared across components.
   *
   * Hook handlers use this pointer to push events:
   * @code
   * event_logger_->push(TimelineEvent{...});
   * @endcode
   */
  EventLogger<1024>* event_logger_;
};

}  // namespace timeline
