# Timeline Hooks Architecture

**Status:** Implemented
**Feature ID:** Real-Time Timeline Visualization
**Last Updated:** 2025-11-01

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Hook Definitions](#hook-definitions)
4. [Integration Points](#integration-points)
5. [Adding New Hooks](#adding-new-hooks)
6. [Testing Hooks](#testing-hooks)
7. [Performance Considerations](#performance-considerations)
8. [Troubleshooting](#troubleshooting)

---

## Overview

The Timeline Hook System provides a **zero-overhead instrumentation pattern** for emitting timeline events from keying subsystem components without creating direct dependencies on the timeline component.

### Design Goals

1. **Zero Code Duplication**: Event emission logic is centralized in `TimelineEventEmitter`
2. **Loose Coupling**: Components like `PaddleEngine` have no timeline component dependency
3. **Single Responsibility**: Timeline concerns are separated from keying logic
4. **Testability**: Hooks can be mocked, emitter can be unit tested independently
5. **Minimal Performance Impact**: Hook calls compile to simple function pointer checks and calls

### Pattern: Hook Callbacks with Context Pointer

The hook pattern follows the same approach as existing `PaddleEngineCallbacks`:

- **Function pointer struct** (`TimelineHooks`) with C-style callbacks
- **Context pointer** (`void*`) passed to all callbacks, points to `TimelineEventEmitter` instance
- **Static handler methods** in `TimelineEventEmitter` that cast context and emit events
- **Dependency injection** via `KeyingSubsystem::SetTimelineEmitter()`

---

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                    ApplicationController                         │
│                                                                  │
│  ┌──────────────────────┐    ┌────────────────────────────┐    │
│  │  KeyingSubsystem     │    │  TimelineEventEmitter      │    │
│  │                      │    │                            │    │
│  │  - EventLogger<4096> │◄───│  - EventLogger* (injected) │    │
│  │  - TimelineEmitter*  │───►│  - GetHooks()              │    │
│  └──────────────────────┘    └────────────────────────────┘    │
│           │                                                      │
│           │ (passes hooks)                                      │
│           ▼                                                      │
│  ┌──────────────────────┐                                       │
│  │  PaddleEngine        │                                       │
│  │                      │                                       │
│  │  - callbacks_        │                                       │
│  │    .timeline_hooks   │                                       │
│  └──────────────────────┘                                       │
└─────────────────────────────────────────────────────────────────┘
```

### Hook Flow Diagram

```
PaddleEngine                KeyingSubsystem         TimelineEventEmitter       EventLogger
    |                              |                         |                      |
    | Memory window opens          |                         |                      |
    |----------------------------->|                         |                      |
    | if (callbacks_.timeline_hooks.OnMemoryWindowChanged != nullptr)              |
    |   callbacks_.timeline_hooks.OnMemoryWindowChanged(..., context)              |
    |                              |                         |                      |
    |                              |------------------------>| HandleMemoryWindow   |
    |                              |   (via context ptr)     |        |             |
    |                              |                         |        v             |
    |                              |                         |   static_cast<...>   |
    |                              |                         |        |             |
    |                              |                         |        v             |
    |                              |                         |   Create TimelineEvent
    |                              |                         |        |             |
    |                              |                         |        v             |
    |                              |                         |   push(event)        |
    |                              |                         |--------------------->|
    |                              |                         |                      |
    |<-----------------------------|<------------------------|                      |
    | (returns)                    |                         |                      |
    |                              |                         |                      |
```

### Data Flow

1. **Initialization** (during application startup):
   ```
   ApplicationController::SubsystemCreationPhase
     └─> Creates TimelineEventEmitter(&keying_subsystem.GetTimeline())

   ApplicationController::SubsystemWiringPhase
     └─> keying_subsystem.SetTimelineEmitter(emitter)
         └─> hooks = emitter->GetHooks()
         └─> paddle_callbacks_.timeline_hooks = hooks
   ```

2. **Runtime** (during keying operations):
   ```
   PaddleEngine::UpdateMemoryState() detects memory window open
     └─> calls callbacks_.timeline_hooks.OnMemoryWindowChanged(...)
         └─> TimelineEventEmitter::HandleMemoryWindowChanged(...)
             └─> Creates TimelineEvent{type: kMemoryWindow, ...}
             └─> event_logger_->push(event)
   ```

---

## Hook Definitions

All hooks are defined in `components/timeline/include/timeline/timeline_hooks.hpp` as function pointers in the `TimelineHooks` struct.

### 1. OnMemoryWindowChanged

**Purpose:** Track iambic memory window state changes (open/close)

**Signature:**
```cpp
void (*OnMemoryWindowChanged)(bool is_dah, bool opened, int64_t timestamp_us, void* context);
```

**Parameters:**
- `is_dah`: `true` if dah memory window, `false` if dit memory window
- `opened`: `true` if window opened, `false` if closed/expired
- `timestamp_us`: Microsecond timestamp from `hal::HighPrecisionClock::NowMicros()`
- `context`: Opaque pointer (cast to `TimelineEventEmitter*`)

**Event Encoding:**
- **type**: `EventType::kMemoryWindow`
- **arg0**: `0` = dit window, `1` = dah window
- **arg1**: `0` = closed, `1` = open

**Called From:**
- File: `components/keying/paddle_engine.cpp`
- Method: `PaddleEngine::UpdateMemoryState()`
- Approximate line: 180-190

**Use Cases:**
- Timeline visualization of memory window active periods
- Debugging iambic timing issues (memory too long/short)
- Analyzing operator keying patterns (memory usage frequency)

---

### 2. OnLateReleaseActive

**Purpose:** Track late release feature activation

**Signature:**
```cpp
void (*OnLateReleaseActive)(uint32_t delay_ms, int64_t timestamp_us, void* context);
```

**Parameters:**
- `delay_ms`: Configured late release delay in milliseconds
- `timestamp_us`: Microsecond timestamp when late release activated
- `context`: Opaque pointer (cast to `TimelineEventEmitter*`)

**Event Encoding:**
- **type**: `EventType::kLateRelease`
- **arg0**: `delay_ms` (late release delay)
- **arg1**: `1` (always 1 to indicate active)

**Called From:**
- File: `components/keying/paddle_engine.cpp`
- Method: `PaddleEngine::HandlePaddleRelease()`
- Approximate line: 220-230

**Use Cases:**
- Timeline visualization of late release activation
- Tuning late release delay parameter (too short/long)
- Analyzing impact of late release on timing accuracy

---

### 3. OnLatchStateChanged

**Purpose:** Track latch state changes (engaged/released)

**Signature:**
```cpp
void (*OnLatchStateChanged)(bool active, int64_t timestamp_us, void* context);
```

**Parameters:**
- `active`: `true` if latch engaged (holding), `false` if released
- `timestamp_us`: Microsecond timestamp of state change
- `context`: Opaque pointer (cast to `TimelineEventEmitter*`)

**Event Encoding:**
- **type**: `EventType::kLatch`
- **arg0**: `0` (unused, reserved for future use)
- **arg1**: `0` = released, `1` = active (latch holding)

**Called From:**
- File: `components/keying/paddle_engine.cpp`
- Method: `PaddleEngine::UpdateLatchState()`
- Approximate line: 250-260

**Use Cases:**
- Timeline visualization of latch active periods
- Debugging latch triggering conditions
- Analyzing operator latch usage patterns

---

### 4. OnSqueezeDetected

**Purpose:** Detect when both paddles are pressed simultaneously

**Signature:**
```cpp
void (*OnSqueezeDetected)(int64_t timestamp_us, void* context);
```

**Parameters:**
- `timestamp_us`: Microsecond timestamp when squeeze detected
- `context`: Opaque pointer (cast to `TimelineEventEmitter*`)

**Event Encoding:**
- **type**: `EventType::kSqueeze`
- **arg0**: `0` (unused, reserved for future use)
- **arg1**: `1` (always 1 to indicate detected)

**Called From:**
- File: `components/keying/paddle_engine.cpp`
- Method: `PaddleEngine::HandlePaddlePress()`
- Approximate line: 140-150

**Use Cases:**
- Timeline visualization of squeeze events (⚡ symbol)
- Diagnosing paddle mechanical problems
- Analyzing operator keying habits (accidental squeezes)
- Supporting squeeze keying mode (future feature)

---

## Integration Points

### 1. ApplicationController (Initialization)

**File:** `components/app/include/app/application_controller.hpp`
**File:** `components/app/init_phases.cpp`

**What:** Creates `TimelineEventEmitter` and wires it into `KeyingSubsystem`

**Code:**
```cpp
// In ApplicationController class (header)
class ApplicationController {
 private:
  std::unique_ptr<timeline::TimelineEventEmitter> timeline_emitter_;

 public:
  timeline::TimelineEventEmitter* GetTimelineEmitter() const {
    return timeline_emitter_.get();
  }
};

// In SubsystemCreationPhase::Execute() (init_phases.cpp)
auto& timeline = controller.keying_subsystem_->GetTimeline();
controller.timeline_emitter_ =
    std::make_unique<timeline::TimelineEventEmitter>(&timeline);

// In SubsystemWiringPhase::Execute() (init_phases.cpp)
controller.keying_subsystem_->SetTimelineEmitter(
    controller.timeline_emitter_.get());
```

---

### 2. KeyingSubsystem (Hook Distribution)

**File:** `components/keying_subsystem/include/keying_subsystem/keying_subsystem.hpp`
**File:** `components/keying_subsystem/keying_subsystem.cpp`

**What:** Receives `TimelineEventEmitter`, extracts hooks, forwards to `PaddleEngine` callbacks

**Code:**
```cpp
// In KeyingSubsystem class (header)
class KeyingSubsystem {
 private:
  timeline::TimelineEventEmitter* timeline_emitter_;

 public:
  void SetTimelineEmitter(timeline::TimelineEventEmitter* emitter);
};

// In implementation (cpp)
void KeyingSubsystem::SetTimelineEmitter(timeline::TimelineEventEmitter* emitter) {
  timeline_emitter_ = emitter;

  if (timeline_emitter_ != nullptr) {
    timeline::TimelineHooks hooks = timeline_emitter_->GetHooks();
    paddle_callbacks_.timeline_hooks = hooks;
  }
}
```

---

### 3. PaddleEngine (Hook Callsites)

**File:** `components/keying/include/keying/paddle_engine.hpp`
**File:** `components/keying/paddle_engine.cpp`

**What:** Extends `PaddleEngineCallbacks` struct, calls hooks at instrumentation points

**Code:**
```cpp
// In paddle_engine.hpp - Extend PaddleEngineCallbacks
struct PaddleEngineCallbacks {
  // ... existing callbacks ...
  timeline::TimelineHooks timeline_hooks;  // NEW: Timeline hooks
};

// In paddle_engine.cpp - Memory window callsite
void PaddleEngine::UpdateMemoryState() {
  // ... memory window logic ...

  if (memory_window_opened) {
    // TIMELINE HOOK: Memory window state change
    if (callbacks_.timeline_hooks.OnMemoryWindowChanged != nullptr) {
      callbacks_.timeline_hooks.OnMemoryWindowChanged(
        is_dah,  // true if dah window
        true,    // opened
        hal::HighPrecisionClock::NowMicros(),
        callbacks_.timeline_hooks.context
      );
    }
  }

  if (memory_window_closed) {
    // TIMELINE HOOK: Memory window state change
    if (callbacks_.timeline_hooks.OnMemoryWindowChanged != nullptr) {
      callbacks_.timeline_hooks.OnMemoryWindowChanged(
        is_dah,  // true if dah window
        false,   // closed
        hal::HighPrecisionClock::NowMicros(),
        callbacks_.timeline_hooks.context
      );
    }
  }
}
```

**Pattern for all hook callsites:**
```cpp
// TIMELINE HOOK: [description]
if (callbacks_.timeline_hooks.HookName != nullptr) {
  callbacks_.timeline_hooks.HookName(
    arg1, arg2, ...,
    hal::HighPrecisionClock::NowMicros(),
    callbacks_.timeline_hooks.context
  );
}
```

---

## Adding New Hooks

Follow these steps to add a new timeline hook:

### Step 1: Define Hook in timeline_hooks.hpp

Edit `components/timeline/include/timeline/timeline_hooks.hpp`:

```cpp
struct TimelineHooks {
  // ... existing hooks ...

  /**
   * @brief New hook description
   * @param param1 Description of parameter 1
   * @param timestamp_us Microsecond timestamp
   * @param context Opaque context pointer (TimelineEventEmitter*)
   */
  void (*OnNewFeature)(uint32_t param1, int64_t timestamp_us, void* context);

  void* context;
};
```

### Step 2: Add Handler to TimelineEventEmitter

Edit `components/timeline/include/timeline/timeline_event_emitter.hpp`:

```cpp
class TimelineEventEmitter {
 public:
  // ... existing handlers ...

  /**
   * @brief Hook callback for new feature
   * ... documentation ...
   */
  static void HandleNewFeature(uint32_t param1, int64_t ts, void* ctx);
};
```

### Step 3: Implement Handler

Edit `components/timeline/timeline_event_emitter.cpp`:

```cpp
void TimelineEventEmitter::HandleNewFeature(uint32_t param1, int64_t ts, void* ctx) {
  auto* emitter = static_cast<TimelineEventEmitter*>(ctx);

  TimelineEvent evt{
    .timestamp_us = ts,
    .type = EventType::kNewFeature,  // Add to enum in event_logger.hpp
    .arg0 = param1,
    .arg1 = 0  // or relevant value
  };

  emitter->event_logger_->push(evt);

  ESP_LOGD(TAG, "New feature: param=%u (%lld us)", param1, (long long)ts);
}
```

### Step 4: Register Hook in GetHooks()

Edit `components/timeline/timeline_event_emitter.cpp`:

```cpp
TimelineHooks TimelineEventEmitter::GetHooks() {
  return TimelineHooks{
    // ... existing hooks ...
    .OnNewFeature = &HandleNewFeature,
    .context = this
  };
}
```

### Step 5: Add EventType Enum Value

Edit `components/timeline/include/timeline/event_logger.hpp`:

```cpp
enum class EventType : uint8_t {
  // ... existing types ...
  kNewFeature = 11,  // arg0: param1, arg1: unused
};
```

### Step 6: Add Hook Callsite

Edit the appropriate component (e.g., `paddle_engine.cpp`):

```cpp
void PaddleEngine::SomeMethod() {
  // ... feature logic ...

  // TIMELINE HOOK: New feature detected
  if (callbacks_.timeline_hooks.OnNewFeature != nullptr) {
    callbacks_.timeline_hooks.OnNewFeature(
      param_value,
      hal::HighPrecisionClock::NowMicros(),
      callbacks_.timeline_hooks.context
    );
  }
}
```

### Step 7: Add Unit Test

Edit `tests_host/test_timeline_event_emitter.cpp`:

```cpp
void test_new_feature_hook() {
  timeline::EventLogger<100> logger;
  timeline::TimelineEventEmitter emitter(&logger);

  auto hooks = emitter.GetHooks();
  hooks.OnNewFeature(42, 1000000, hooks.context);

  assert(logger.size() == 1);
  auto evt = logger.latest();
  assert(evt.type == timeline::EventType::kNewFeature);
  assert(evt.arg0 == 42);

  std::cout << "test_new_feature_hook PASSED\n";
}
```

### Step 8: Update Documentation

- Update this file (`docs/TIMELINE_HOOKS.md`) with new hook details
- Update API handler to map new event type to JSON string
- Update frontend to render new event type (if applicable)

---

## Testing Hooks

### Unit Testing (Host Tests)

Unit tests verify hook callbacks emit correct events to EventLogger.

**File:** `tests_host/test_timeline_event_emitter.cpp`

**Pattern:**
```cpp
void test_hook_name() {
  // 1. Create EventLogger
  timeline::EventLogger<100> logger;

  // 2. Create TimelineEventEmitter with injected logger
  timeline::TimelineEventEmitter emitter(&logger);

  // 3. Get hooks struct
  auto hooks = emitter.GetHooks();

  // 4. Call hook
  hooks.OnMemoryWindowChanged(false, true, 1000000, hooks.context);

  // 5. Verify event in logger
  assert(logger.size() == 1);
  auto evt = logger.latest();
  assert(evt.type == timeline::EventType::kMemoryWindow);
  assert(evt.arg0 == 0);  // dit
  assert(evt.arg1 == 1);  // open
  assert(evt.timestamp_us == 1000000);

  std::cout << "test_hook_name PASSED\n";
}
```

**Run tests:**
```bash
./run_tests.sh
```

---

### Integration Testing (Hardware)

Integration tests verify hooks are called from actual keying operations.

**Steps:**

1. **Build with debug logging:**
   ```bash
   idf.py menuconfig
   # Component config → Log output → Default log verbosity → Debug
   idf.py build
   ```

2. **Flash firmware:**
   ```bash
   idf.py flash
   ```

3. **Monitor serial output:**
   ```bash
   idf.py monitor
   ```

4. **Trigger keying events:**
   - Press dit paddle → Should see `[DEBUG][TimelineEmitter] Memory window DIT: OPEN`
   - Release dit paddle → Should see `[DEBUG][TimelineEmitter] Memory window DIT: CLOSED`
   - Press both paddles → Should see `[DEBUG][TimelineEmitter] Squeeze detected`

5. **Verify events in timeline:**
   - Connect to device WiFi
   - Open `http://<device-ip>/timeline`
   - Press paddles and verify events appear on canvas

---

## Performance Considerations

### Overhead Analysis

1. **Hook call overhead:**
   - Null check: ~1-2 CPU cycles
   - Function pointer call: ~5-10 CPU cycles
   - Total per hook: <50 CPU cycles (~0.5 µs @ 80 MHz)

2. **Event emission overhead:**
   - Critical section lock: ~10-20 CPU cycles
   - Event struct copy: ~20 CPU cycles
   - Circular buffer update: ~10 CPU cycles
   - Total per event: <200 CPU cycles (~2 µs @ 80 MHz)

3. **Total impact:**
   - Typical keying operation: 10-50 ms duration
   - Hook overhead: <0.01% of operation time
   - Negligible impact on timing accuracy

### Optimization Tips

1. **Use null hook pattern:**
   - Always check `if (hook != nullptr)` before calling
   - Compiler optimizes out call when hook not set

2. **Minimize work in handlers:**
   - Static handlers should only create event and push to logger
   - No complex logic, no allocations, no blocking operations

3. **ISR-safe considerations:**
   - EventLogger::push() uses critical sections (ISR-safe)
   - ESP_LOGD() may allocate (avoid in ISR context)
   - Consider push_isr() for ISR context hooks

---

## Troubleshooting

### Hook Not Called

**Symptom:** Expected timeline events don't appear, no ESP_LOGD output

**Possible causes:**

1. **Timeline emitter not wired:**
   - Check `ApplicationController::SubsystemWiringPhase` calls `SetTimelineEmitter()`
   - Verify `KeyingSubsystem::SetTimelineEmitter()` forwards hooks to callbacks

2. **Hook pointer null:**
   - Verify `TimelineEventEmitter::GetHooks()` sets all function pointers
   - Check `PaddleEngineCallbacks::timeline_hooks` is initialized

3. **Hook callsite not reached:**
   - Add temporary log before hook call to verify code path is executed
   - Check feature is enabled (e.g., memory window, late release)

**Debug steps:**
```cpp
// In PaddleEngine callsite:
ESP_LOGI("DEBUG", "About to call hook, ptr=%p",
         callbacks_.timeline_hooks.OnMemoryWindowChanged);
if (callbacks_.timeline_hooks.OnMemoryWindowChanged != nullptr) {
  ESP_LOGI("DEBUG", "Calling hook now");
  callbacks_.timeline_hooks.OnMemoryWindowChanged(...);
}
```

---

### Events Missing from Timeline

**Symptom:** ESP_LOGD shows hook called, but events don't appear in Web UI

**Possible causes:**

1. **EventLogger overflow:**
   - Check `EventLogger::dropped_count()` in API handler
   - Increase EventLogger capacity in template parameter

2. **API handler not mapping event type:**
   - Verify `HandleGetTimelineEvents()` maps `EventType::kMemoryWindow` to JSON string
   - Check frontend recognizes event type string

3. **Timestamp issues:**
   - Verify `hal::HighPrecisionClock::NowMicros()` returns valid timestamps
   - Check frontend `since` parameter in API polling

**Debug steps:**
```cpp
// In TimelineEventEmitter handler:
ESP_LOGI("DEBUG", "Event pushed: type=%d, size=%zu, dropped=%zu",
         static_cast<int>(evt.type),
         emitter->event_logger_->size(),
         emitter->event_logger_->dropped_count());
```

---

### Compilation Errors

**Symptom:** Build fails with undefined reference to hook functions

**Possible causes:**

1. **Missing `timeline_event_emitter.cpp` in CMakeLists.txt:**
   - Check `components/timeline/CMakeLists.txt` includes source file

2. **Header not included:**
   - Verify `#include "timeline/timeline_hooks.hpp"` in `paddle_engine.cpp`
   - Add forward declaration in `keying_subsystem.hpp`

3. **Circular dependency:**
   - Timeline component should NOT depend on keying components
   - Keying components should only depend on timeline headers (not link timeline lib)

**Fix:**
```cmake
# In components/keying/CMakeLists.txt
idf_component_register(
  SRCS "paddle_engine.cpp"
  INCLUDE_DIRS "include"
  REQUIRES timeline  # Add timeline to access hooks header
)
```

---

## References

- **Hook Definitions:** `components/timeline/include/timeline/timeline_hooks.hpp`
- **Emitter Implementation:** `components/timeline/timeline_event_emitter.cpp`
- **Unit Tests:** `tests_host/test_timeline_event_emitter.cpp`
- **PRD:** `.project-management/current-prd/prd-realtime-timeline.md`
- **Task List:** `.project-management/current-prd/tasks-prd-realtime-timeline.md`

---

*End of document*
