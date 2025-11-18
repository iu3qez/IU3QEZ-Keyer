# Keying Subsystem Component

## Purpose
System integration layer that connects paddle hardware/OS concerns with CW keying logic.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    KeyingSubsystem                          │
│  (OS Integration: ISR, Queue, Timeline, Engine Lifecycle)  │
└─────────────────┬───────────────────────────────────────────┘
                  │
                  ├─> FreeRTOS Queue (ISR → main loop)
                  ├─> timeline::EventLogger (diagnostics)
                  └─> keying::PaddleEngine (pure CW logic)
```

### Component Separation Rationale

| Component | Responsibility | Dependencies |
|-----------|---------------|--------------|
| **keying::PaddleEngine** | Pure CW algorithm (iambic logic, timing) | None (testable on host) |
| **KeyingSubsystem** | OS integration (ISR, queues, timeline) | FreeRTOS, ESP-IDF HAL |

**Why separate?**
- **Testability**: PaddleEngine can be unit-tested without FreeRTOS
- **Reusability**: PaddleEngine could be used in remote keyer, simulator
- **Maintainability**: Algorithm changes independent from queue/ISR management

## Responsibilities

1. **ISR Event Capture**: Provide `RecordPaddleEvent()` callback for PaddleHal
2. **Queue Management**: Create/manage FreeRTOS queue for deferred processing
3. **Engine Lifecycle**: Initialize PaddleEngine from DeviceConfig
4. **Timeline Logging**: Record paddle events and keying elements
5. **Runtime Config**: Expose engine reference for console command updates

## Usage Pattern

```cpp
#include "keying_subsystem/keying_subsystem.hpp"

// Initialization
KeyingSubsystem keying;
keying.Initialize(device_config);  // Setup queue + engine + callbacks

// Register ISR callback with HAL
paddle_hal.Initialize(hal_config, 
                      KeyingSubsystem::RecordPaddleEvent, 
                      &keying);

// Main loop
while (true) {
  keying.DrainPaddleEvents();  // Process ISR queue → engine
  keying.Tick(now_us);          // Update engine → emit keying elements
  vTaskDelay(pdMS_TO_TICKS(20));
}
```

## API Summary

### Initialization
- `esp_err_t Initialize(const config::DeviceConfig&)` - Setup queue, engine, callbacks

### Runtime Processing
- `void DrainPaddleEvents()` - Process queued paddle events (call from main loop)
- `void Tick(int64_t now_us)` - Update engine state and timing

### ISR Callback
- `static void RecordPaddleEvent(const hal::PaddleEvent&, void*)` - ISR-safe event capture

### Access Methods
- `keying::PaddleEngine& GetEngine()` - Direct engine access for config updates
- `timeline::EventLogger& GetTimeline()` - Timeline access for diagnostics
- `uint32_t GetDroppedEventCount()` - Queue overflow counter

## Thread Safety

- **ISR Context**: `RecordPaddleEvent()` is IRAM-safe, uses FreeRTOS queue
- **Main Loop**: All other methods must be called from single task context
- **Queue**: Automatically handles ISR → task synchronization

## Memory Footprint

- Paddle event queue: 32 slots × ~20 bytes = 640 bytes
- Timeline logger: 1024 events × 16 bytes = 16 KB
- Engine state: ~200 bytes
- **Total**: ~17 KB

## Dependencies

- `config` - DeviceConfig structure
- `keying` - PaddleEngine algorithm
- `timeline` - EventLogger
- `hal` - PaddleEvent, HighPrecisionClock
- `freertos` - Queue, task primitives

## Related Components

- `components/keying/` - Pure CW algorithm (no OS dependencies)
- `components/keyer_hal/` - Hardware GPIO/ISR management
- `components/timeline/` - Ring buffer event logger
- `components/app/` - Application orchestrator (uses this subsystem)
