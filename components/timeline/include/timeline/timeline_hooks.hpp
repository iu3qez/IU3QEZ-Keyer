#pragma once

#include <cstdint>

/**
 * @file timeline_hooks.hpp
 * @brief Timeline event emission hook system for zero-overhead instrumentation
 *
 * ARCHITECTURE OVERVIEW
 * =====================
 * This file defines a hook-based pattern for emitting timeline events from various
 * keying subsystem components WITHOUT creating direct dependencies on the timeline
 * component. This achieves:
 *
 * 1. **Zero code duplication**: Event emission logic is centralized in TimelineEventEmitter
 * 2. **Loose coupling**: Components like PaddleEngine have no timeline component dependency
 * 3. **Single responsibility**: Timeline concerns are separated from keying logic
 * 4. **Testability**: Hooks can be mocked, emitter can be unit tested independently
 *
 * INTEGRATION PATTERN
 * ===================
 * The hook pattern follows the same approach as existing PaddleEngineCallbacks:
 *
 * 1. TimelineEventEmitter implements static handler methods (HandleMemoryWindowChanged, etc.)
 * 2. TimelineEventEmitter::GetHooks() returns a TimelineHooks struct with:
 *    - Function pointers set to the static handlers
 *    - Context pointer set to the emitter instance (this)
 * 3. ApplicationController creates TimelineEventEmitter during initialization
 * 4. KeyingSubsystem receives the hooks via SetTimelineEmitter()
 * 5. PaddleEngine receives hooks via extended PaddleEngineCallbacks struct
 * 6. PaddleEngine calls hooks at strategic instrumentation points
 * 7. Static handlers cast context back to TimelineEventEmitter* and emit events
 *
 * HOOK FLOW DIAGRAM
 * =================
 *   PaddleEngine              KeyingSubsystem         TimelineEventEmitter       EventLogger
 *        |                            |                         |                      |
 *        | hook call                  |                         |                      |
 *        |--------------------------->|------------------------>| HandleMemoryWindow   |
 *        |   (via callback)           |   (via context ptr)     |        |             |
 *        |                            |                         |        v             |
 *        |                            |                         |   push(event)        |
 *        |                            |                         |--------------------->|
 *        |                            |                         |                      |
 *
 * EXAMPLE USAGE
 * =============
 *
 * // In TimelineEventEmitter.cpp:
 * TimelineHooks TimelineEventEmitter::GetHooks() {
 *   return TimelineHooks{
 *     .OnMemoryWindowChanged = &HandleMemoryWindowChanged,
 *     .OnLatchStateChanged = &HandleLatchStateChanged,
 *     .OnSqueezeDetected = &HandleSqueezeDetected,
 *     .context = this
 *   };
 * }
 *
 * // In PaddleEngine.cpp (at memory window state change):
 * if (callbacks_.timeline_hooks.OnMemoryWindowChanged != nullptr) {
 *   callbacks_.timeline_hooks.OnMemoryWindowChanged(
 *     false,  // is_dah = false (dit window)
 *     true,   // opened = true
 *     hal::HighPrecisionClock::NowMicros(),
 *     callbacks_.timeline_hooks.context
 *   );
 * }
 *
 * ADDING NEW HOOKS
 * ================
 * To add a new timeline hook:
 *
 * 1. Add function pointer to TimelineHooks struct below
 * 2. Add static handler method to TimelineEventEmitter class
 * 3. Implement handler in timeline_event_emitter.cpp
 * 4. Add hook assignment in TimelineEventEmitter::GetHooks()
 * 5. Add hook callsite in appropriate component (e.g., PaddleEngine)
 * 6. Document the new hook in docs/TIMELINE_HOOKS.md
 * 7. Add unit test for the new hook
 *
 * @see docs/TIMELINE_HOOKS.md for detailed integration documentation
 * @see components/timeline/timeline_event_emitter.hpp for hook implementations
 */

namespace timeline {

/**
 * @brief Timeline event emission hooks for instrumentation without dependencies
 *
 * This struct contains function pointers for all timeline event hooks.
 * All function pointers may be nullptr (null hook pattern - no-op if not set).
 * The context pointer is passed to all callbacks and should point to the
 * TimelineEventEmitter instance that handles event emission.
 */
struct TimelineHooks {
  /**
   * @brief Called when iambic memory window state changes (opened/closed)
   *
   * The memory window is a timing window during which a paddle press is
   * "remembered" for iambic keying. This hook is called when:
   * - Memory window opens (operator presses opposite paddle during element)
   * - Memory window closes (memory timeout expires or operator releases)
   *
   * **When called:**
   * - PaddleEngine::UpdateMemoryState() when memory window timer starts/expires
   * - Approximate location: components/keying/paddle_engine.cpp:180-190
   *
   * **Use cases:**
   * - Timeline visualization of memory window active periods
   * - Debugging iambic timing issues (memory too long/short)
   * - Analyzing operator keying patterns (memory usage frequency)
   *
   * @param is_dah true if dah memory window, false if dit memory window
   * @param opened true if window opened, false if window closed/expired
   * @param timestamp_us microsecond timestamp from HighPrecisionClock
   * @param context opaque context pointer (cast to TimelineEventEmitter*)
   */
  void (*OnMemoryWindowChanged)(bool is_dah, bool opened, int64_t timestamp_us, void* context);

  /**
   * @brief Called when latch state changes (engaged/released)
   *
   * Latch holds the paddle state active even after physical release,
   * useful for continuous sending. This hook is called when:
   * - Latch engages (paddle held active)
   * - Latch releases (returns to normal operation)
   *
   * **When called:**
   * - PaddleEngine::UpdateLatchState() when latch engages/releases
   * - Approximate location: components/keying/paddle_engine.cpp:250-260
   *
   * **Use cases:**
   * - Timeline visualization of latch active periods
   * - Debugging latch triggering conditions
   * - Analyzing operator latch usage patterns
   *
   * @param active true if latch engaged (holding), false if latch released
   * @param timestamp_us microsecond timestamp of state change
   * @param context opaque context pointer (cast to TimelineEventEmitter*)
   */
  void (*OnLatchStateChanged)(bool active, int64_t timestamp_us, void* context);

  /**
   * @brief Called when both paddles pressed simultaneously (squeeze detected)
   *
   * Squeeze detection identifies when the operator presses both dit and dah
   * paddles at the same time, which can indicate:
   * - Intentional squeeze keying technique
   * - Accidental simultaneous press (operator error)
   * - Paddle mechanical issues (contacts too close)
   *
   * **When called:**
   * - PaddleEngine::HandlePaddlePress() when both paddles transition to pressed
   * - Approximate location: components/keying/paddle_engine.cpp:140-150
   *
   * **Use cases:**
   * - Timeline visualization of squeeze events (⚡ symbol)
   * - Diagnosing paddle mechanical problems
   * - Analyzing operator keying habits (accidental squeezes)
   * - Supporting squeeze keying mode (future feature)
   *
   * @param timestamp_us microsecond timestamp when squeeze detected
   * @param context opaque context pointer (cast to TimelineEventEmitter*)
   */
  void (*OnSqueezeDetected)(int64_t timestamp_us, void* context);

  /**
   * @brief Called when CW keying state changes (key on/off)
   *
   * This hook is called when the generated CW output changes state, representing
   * the actual transmitted signal (not raw paddle events). This is the signal
   * that drives audio sidetone, transmitter keying, and remote keying.
   *
   * **When called:**
   * - KeyingSubsystem callback when PaddleEngine emits keying event
   * - Approximate location: components/keying_subsystem/keying_subsystem.cpp:200-210
   *
   * **Use cases:**
   * - Remote CW transmission (CWNet protocol)
   * - Timeline visualization of transmitted CW
   * - Audio sidetone triggering
   * - TX hardware control
   *
   * @param key_active true if CW key closed (transmitting), false if key open
   * @param timestamp_us microsecond timestamp of keying state change
   * @param context opaque context pointer (cast to TimelineEventEmitter*)
   */
  void (*OnKeyingChanged)(bool key_active, int64_t timestamp_us, void* context);

  /**
   * @brief Opaque context pointer passed to all callback functions
   *
   * This pointer should be set to the TimelineEventEmitter instance that
   * owns these hooks. Callbacks cast this back to TimelineEventEmitter*
   * to access the event logger and emit events.
   *
   * **Pattern:**
   * ```cpp
   * // In callback handler:
   * auto* emitter = static_cast<TimelineEventEmitter*>(context);
   * emitter->event_logger_->push(event);
   * ```
   */
  void* context;
};

}  // namespace timeline
