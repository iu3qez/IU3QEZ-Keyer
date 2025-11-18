# RemoteCwClient Task-Based Architecture Design

**Date:** 2025-11-15
**Author:** AI Assistant (Brainstorming Session)
**Status:** Approved for Implementation
**Related Bug Report:** `docs/BUG_REPORT_RemoteCwClient_Architecture.md`

---

## Executive Summary

Refactor RemoteCwClient from polling-based architecture (1000 Hz, 42.7% main loop CPU) to event-driven FreeRTOS task with `select()` blocking I/O. This eliminates excessive CPU usage, fixes initialization bugs, and integrates with Timeline system.

**Performance Impact:**
- Main loop CPU: 42.7% → ~0% (RemoteCw moved to dedicated task)
- Task CPU: ~4.7% continuous → <1% when active, 0% when idle
- Total system CPU freed: ~4%

**Architectural Decision:**
- **Violate PRD TC-5** ("SHALL NOT create additional tasks")
- **Rationale:** Remote keying is critical system element, deserves dedicated resources
- **Requires:** PRD update to reflect new architecture

---

## Problems Solved

### Bug #1: Excessive Socket Polling
**Before:** `HandleSocketIo()` called 1000 times/second from main loop, `lwip_recv()` consumes 47 µs per call even when no data available.

**After:** Task uses `select()` with 50ms timeout. CPU idle when no socket activity.

### Bug #2: Initialization Logic Error
**Before:** Client starts even when `config_.remote.enabled == false` due to missing early return in init phase.

**After:** Init phase respects `enabled` flag with explicit early return.

### Bug #3: Missing Timeline Integration
**Before:** RemoteCwClient maintains separate `key_queue_[]` for paddle events, duplicating Timeline data.

**After:** RemoteCwClient consumes paddle events from Timeline via hook callback, eliminates duplication.

---

## High-Level Architecture

### Component Overview

**RemoteCwClient becomes a two-component system:**

1. **RemoteCwClientTask** (new FreeRTOS task)
   - Priority: `tskIDLE_PRIORITY + 2` (above idle, below main loop)
   - Stack: 4096 bytes (DNS, TCP, frame parsing)
   - Core: Any (ESP32-S3 dual core, let scheduler decide)
   - Lifetime: Created in `Configure()`, runs forever (even when idle)
   - Autonomy: Manages state machine, socket, networking independently

2. **RemoteCwClient** (refactored API facade)
   - Lives in main loop context
   - Simplified API: `Start()`, `Stop()`, `GetState()`, `GetLatency()`
   - Communicates with task via command queue
   - Thread-safe state access via `std::atomic`

### Data Flow: Paddle → Remote Server

```
Paddle ISR → Timeline (main loop context)
               ↓ (hook callback invoked, ISR-safe)
           FreeRTOS Queue (thread-safe handoff)
               ↓ (task dequeues events)
   RemoteCwClientTask (task context)
               ↓ (encode CWNet protocol frames)
           select() + send() → Remote Server
```

### Data Flow: Remote Server → Task (Future)

**Note:** RemoteCwServer receiving logic ignored for now (separate component, untested).

For client-only mode, remote server may send PING, PRINT, CONNECT ACK:
```
Remote Server → select() detects data available
               ↓ (recv() in task context)
   RemoteCwClientTask (parse CWNet frames)
               ↓ (update state: latency, connection status)
           Atomic state variables
               ↓ (atomic reads, lock-free)
   Main Loop (GetLatency(), GetState() for UI)
```

---

## Inter-Task Communication

### Timeline Hook → Task (Paddle Events)

**Hook Registration (during Configure):**

```cpp
void RemoteCwClient::SetTimeline(timeline::TimelineLogger* timeline) {
  timeline_ = timeline;

  timeline::HookRegistration hook{
    .on_paddle_event = &RemoteCwClient::OnPaddleEventHook,
    .context = this  // Pass RemoteCwClient instance as context
  };
  timeline_->RegisterHook(hook);
}
```

**Hook Callback (ISR-safe):**

```cpp
// Static callback invoked by Timeline (may be called from ISR!)
static void OnPaddleEventHook(bool key_down, int64_t timestamp_us, void* ctx) {
  RemoteCwClient* self = static_cast<RemoteCwClient*>(ctx);

  PaddleEvent evt{key_down, timestamp_us};
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  // ISR-safe queue send (non-blocking)
  xQueueSendFromISR(self->paddle_queue_, &evt, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

**Paddle Event Queue:**
- Type: `QueueHandle_t` (FreeRTOS native)
- Item: `struct PaddleEvent { bool key_down; int64_t timestamp_us; };`
- Item size: 16 bytes (bool + int64_t + alignment padding)
- Capacity: 64 events (matches current `kMaxKeyQueueDepth`)
- Send: Non-blocking from ISR (`xQueueSendFromISR`)
- Receive: Timeout receive in task (`xQueueReceive(..., 0)` for non-blocking drain)

**Overflow Handling:**
- If queue full, drop event and increment atomic counter `dropped_paddle_events_`
- Log warning (rate-limited to avoid log spam)
- Diagnostics can expose counter via Web UI

### Main Loop → Task (Commands)

**Command Enum:**

```cpp
enum class TaskCommand : uint8_t {
  kStart,         // Begin connection attempts
  kStop,          // Close socket, return to idle
  kUpdateConfig,  // Reconfigure server/port/callsign (while stopped)
};

struct CommandMessage {
  TaskCommand cmd;
  union {
    RemoteCwClientConfig config;  // For kUpdateConfig
  } payload;
};
```

**Command Queue:**
- Type: `QueueHandle_t`
- Item size: `sizeof(CommandMessage)` (~100 bytes due to config union)
- Capacity: 4 commands (small, commands are infrequent)
- Main loop: `xQueueSend(cmd_queue_, &cmd, pdMS_TO_TICKS(100))` (blocking with 100ms timeout)
- Task: `xQueueReceive(cmd_queue_, &cmd, 0)` (non-blocking check every loop iteration)

**API Methods:**

```cpp
esp_err_t RemoteCwClient::Start() {
  CommandMessage cmd{TaskCommand::kStart};
  if (xQueueSend(cmd_queue_, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;  // Queue full (unlikely)
  }
  return ESP_OK;
}

void RemoteCwClient::Stop() {
  CommandMessage cmd{TaskCommand::kStop};
  xQueueSend(cmd_queue_, &cmd, portMAX_DELAY);  // Block until accepted
}
```

---

## Task Loop Design

### Task Entry Point

```cpp
// Static function registered with xTaskCreate
static void TaskFunction(void* param) {
  RemoteCwClient* self = static_cast<RemoteCwClient*>(param);
  self->RunTaskLoop();  // Never returns (infinite loop)
}
```

### Main Loop Structure

```cpp
void RemoteCwClient::RunTaskLoop() {
  ESP_LOGI(TAG, "RemoteCwClient task started");

  while (true) {
    // 1. Process commands from main loop (non-blocking)
    ProcessCommandQueue();

    // 2. State machine progression
    switch (state_.load(std::memory_order_acquire)) {
      case RemoteCwClientState::kIdle:
        // Wait for kStart command (long timeout to yield CPU)
        WaitForCommandOrTimeout(1000);  // 1 second
        break;

      case RemoteCwClientState::kResolving:
        AttemptDnsResolution();  // Blocking getaddrinfo() - OK in task!
        if (resolved_addr_valid_) {
          TransitionTo(RemoteCwClientState::kConnecting);
        } else {
          ScheduleRetry();  // Sets next_reconnect_time_us_, moves to kError
        }
        break;

      case RemoteCwClientState::kConnecting:
        AttemptConnect();  // Non-blocking connect, polls SO_ERROR
        if (connect_successful_) {
          TransitionTo(RemoteCwClientState::kHandshake);
        } else if (connect_timeout_) {
          ScheduleRetry();
        }
        // else: still in progress, check again next iteration
        break;

      case RemoteCwClientState::kHandshake:
      case RemoteCwClientState::kConnected:
        HandleConnectedState();  // Event-driven I/O (details below)
        break;

      case RemoteCwClientState::kError:
        WaitForRetry();  // select() timeout until next_reconnect_time_us_
        if (TimeToRetry()) {
          TransitionTo(RemoteCwClientState::kResolving);
        }
        break;
    }
  }
}
```

### HandleConnectedState() - Event-Driven I/O Core

```cpp
void RemoteCwClient::HandleConnectedState() {
  // Setup select() file descriptor sets
  fd_set readfds, writefds;
  FD_ZERO(&readfds);
  FD_ZERO(&writefds);

  FD_SET(socket_fd_, &readfds);  // Always monitor for incoming data

  if (HasDataToSend()) {
    FD_SET(socket_fd_, &writefds);  // Monitor for write-ready if TX pending
  }

  // Timeout: 50ms (allows periodic tasks like ping checks)
  struct timeval timeout{0, 50000};  // 50ms = 50000 microseconds

  int ready = select(socket_fd_ + 1, &readfds, &writefds, NULL, &timeout);

  if (ready > 0) {
    // Socket ready for I/O
    if (FD_ISSET(socket_fd_, &readfds)) {
      HandleSocketRead();   // recv() and parse incoming frames
    }
    if (FD_ISSET(socket_fd_, &writefds)) {
      HandleSocketWrite();  // send() from TX buffer
    }
  } else if (ready == 0) {
    // Timeout (50ms elapsed, no socket activity)
    // Fall through to drain paddle queue and periodic tasks
  } else {
    // Error in select()
    ESP_LOGE(TAG, "select() failed: errno=%d", errno);
    HandleSocketError(errno);
    return;
  }

  // Drain paddle events from queue (called every iteration, timeout or not)
  DrainPaddleQueue();

  // Periodic tasks (ping interval, handshake timeout, PTT tail)
  CheckPeriodicTasks();
}
```

**Key Characteristics:**
- `select()` with 50ms timeout → task yields CPU when socket idle
- Paddle queue drained every iteration (max 50ms latency paddle→send)
- Socket activity triggers immediate I/O (sub-millisecond response)
- Zero CPU when both socket and paddle queue idle

### DrainPaddleQueue() Implementation

```cpp
void RemoteCwClient::DrainPaddleQueue() {
  PaddleEvent evt;

  // Drain all events currently in queue (non-blocking)
  while (xQueueReceive(paddle_queue_, &evt, 0) == pdTRUE) {
    // Encode CWNet MORSE frame
    uint32_t delta_ms = CalculateDeltaMs(evt.timestamp_us, last_local_key_timestamp_us_);
    uint8_t cw_byte = EncodeTimestamp(delta_ms) | (evt.key_down ? 0x80 : 0x00);

    // Queue frame to TX buffer (ring buffer, same as before)
    QueueMorseFrame(cw_byte);

    last_local_key_timestamp_us_ = evt.timestamp_us;
    last_keying_activity_us_ = esp_timer_get_time();
  }

  // If we sent any keying events, ensure PTT is on
  if (HasKeyingActivity() && !ptt_active_) {
    SendPttCommand(true);
  }
}
```

---

## Thread Safety & Synchronization

### Atomic State Variables

**Problem:** Main loop reads state/latency, task writes them.

**Solution:** C++11 `std::atomic` (header-only, no STL runtime dependency)

```cpp
class RemoteCwClient {
 private:
  std::atomic<RemoteCwClientState> state_{RemoteCwClientState::kIdle};
  std::atomic<uint32_t> measured_latency_ms_{0};
  std::atomic<uint32_t> dropped_paddle_events_{0};  // Diagnostics counter

 public:
  // Main loop API (thread-safe reads, lock-free)
  RemoteCwClientState GetState() const {
    return state_.load(std::memory_order_relaxed);
  }

  uint32_t GetLatency() const {
    return measured_latency_ms_.load(std::memory_order_relaxed);
  }

  uint32_t GetDroppedEventCount() const {
    return dropped_paddle_events_.load(std::memory_order_relaxed);
  }

  // Task-only API (writes)
  void TransitionTo(RemoteCwClientState new_state) {
    RemoteCwClientState old_state = state_.exchange(new_state, std::memory_order_release);

    if (old_state != new_state) {
      ESP_LOGI(TAG, "State: %s -> %s", StateToString(old_state), StateToString(new_state));
    }
  }

  void UpdateLatency(uint32_t latency_ms) {
    measured_latency_ms_.store(latency_ms, std::memory_order_release);
  }
};
```

**Memory Ordering:**
- `relaxed`: Sufficient for status reads (no ordering requirements)
- `release`: Write barrier ensures state visible to other cores

### Queue Thread Safety

**FreeRTOS queues are inherently thread-safe:**
- `xQueueSendFromISR()`: ISR → Task (paddle events)
- `xQueueSend()`: Main loop → Task (commands)
- `xQueueReceive()`: Task dequeues (blocking or non-blocking)

No additional synchronization needed.

---

## Error Handling & Recovery

### Socket Errors

```cpp
void RemoteCwClient::HandleSocketError(int err) {
  ESP_LOGE(TAG, "Socket error: %s (errno=%d)", strerror(err), err);

  CloseSocket();
  ClearBuffers();
  ScheduleRetry(config_.reconnect_delay_ms);
  TransitionTo(RemoteCwClientState::kError);
}
```

**Triggering Conditions:**
- `recv() == 0` → Server closed connection
- `recv() < 0 && errno != EWOULDBLOCK` → Socket error
- `send() < 0 && errno != EWOULDBLOCK` → Transmission error
- `select() < 0` → Select failure

### DNS Resolution Failures

```cpp
void RemoteCwClient::AttemptDnsResolution() {
  struct addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_INET;

  struct addrinfo* result = nullptr;
  int err = getaddrinfo(config_.server_host, nullptr, &hints, &result);

  if (err != 0 || result == nullptr) {
    ESP_LOGE(TAG, "DNS resolution failed for %s: %d", config_.server_host, err);
    resolved_addr_valid_ = false;
    if (result) freeaddrinfo(result);
    ScheduleRetry(config_.reconnect_delay_ms);
    return;
  }

  // Success: copy resolved address
  memcpy(&resolved_addr_, result->ai_addr, result->ai_addrlen);
  resolved_addr_len_ = result->ai_addrlen;
  resolved_addr_valid_ = true;
  freeaddrinfo(result);
}
```

**Note:** `getaddrinfo()` is blocking (may take seconds). This is acceptable in dedicated task, but would block main loop in old architecture!

### Queue Overflow (Paddle Events)

```cpp
// In Timeline hook callback (ISR context)
static void OnPaddleEventHook(bool key_down, int64_t timestamp_us, void* ctx) {
  RemoteCwClient* self = static_cast<RemoteCwClient*>(ctx);

  PaddleEvent evt{key_down, timestamp_us};
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  if (xQueueSendFromISR(self->paddle_queue_, &evt, &xHigherPriorityTaskWoken) != pdTRUE) {
    // Queue full - drop event
    self->dropped_paddle_events_.fetch_add(1, std::memory_order_relaxed);

    // Rate-limited warning (avoid log spam)
    static int64_t last_warning_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_warning_us > 1000000) {  // 1 second
      ESP_LOGW(TAG, "Paddle event queue overflow (dropped events: %lu)",
               self->dropped_paddle_events_.load());
      last_warning_us = now_us;
    }
  }

  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

**Recovery:** Dropped events are logged and counted, but system continues. Queue depth (64) is sufficient for normal operation (>7 seconds at 9 events/sec worst case).

### Handshake Timeout

```cpp
void RemoteCwClient::CheckPeriodicTasks() {
  int64_t now_us = esp_timer_get_time();

  // Handshake timeout check (only in kHandshake state)
  if (state_.load() == RemoteCwClientState::kHandshake) {
    int64_t elapsed_ms = (now_us - state_enter_time_us_) / 1000;
    if (elapsed_ms > config_.handshake_timeout_ms) {
      ESP_LOGE(TAG, "Handshake timeout after %lld ms", elapsed_ms);
      HandleSocketError(ETIMEDOUT);
      return;
    }
  }

  // Other periodic tasks (ping interval, PTT timeout) in kConnected state
  if (state_.load() == RemoteCwClientState::kConnected && handshake_complete_) {
    CheckPingInterval(now_us);
    CheckPttTimeout(now_us);
  }
}
```

### Task Shutdown (Stop Command)

```cpp
void RemoteCwClient::ProcessCommandQueue() {
  CommandMessage cmd;

  // Non-blocking receive (check every loop iteration)
  if (xQueueReceive(cmd_queue_, &cmd, 0) == pdTRUE) {
    switch (cmd.cmd) {
      case TaskCommand::kStart:
        if (state_.load() == RemoteCwClientState::kIdle) {
          TransitionTo(RemoteCwClientState::kResolving);
        }
        break;

      case TaskCommand::kStop:
        CloseSocket();
        ClearQueues();
        ClearBuffers();
        TransitionTo(RemoteCwClientState::kIdle);
        ESP_LOGI(TAG, "Stopped by command");
        break;

      case TaskCommand::kUpdateConfig:
        if (state_.load() == RemoteCwClientState::kIdle) {
          config_ = cmd.payload.config;
          ESP_LOGI(TAG, "Config updated");
        } else {
          ESP_LOGW(TAG, "Cannot update config while active - stop first");
        }
        break;
    }
  }
}
```

**Note:** Task never exits. After `Stop`, it remains in `kIdle` state, waiting for next `Start` command.

---

## Integration With Existing System

### ApplicationController Changes

**File:** `components/app/init_phases.cpp`

**RemoteClientPhase::Execute() - FIX Bug #2:**

```cpp
esp_err_t RemoteClientPhase::Execute() {
  // FIX: Respect enabled flag with early return!
  if (!config_.remote.enabled) {
    ESP_LOGI(kLogTag, "Remote CW client disabled (remote.enabled=false)");
    return ESP_OK;  // Don't create, configure, or start
  }

  ESP_LOGI(kLogTag, "Initializing Remote CW client...");

  // Create RemoteCwClient (includes task creation)
  client_ = std::make_unique<remote::RemoteCwClient>();

  // Configure
  remote::RemoteCwClientConfig cfg{
    .server_host = config_.remote.server_host,
    .server_port = config_.remote.server_port,
    .callsign = config_.general.callsign,
    .reconnect_delay_ms = 5000,
    .ping_interval_ms = 2000,
    .handshake_timeout_ms = 3000,
    .ptt_tail_ms = config_.remote.ptt_tail_ms,
  };

  client_->Configure(cfg);

  // FIX Bug #3: Wire to Timeline for paddle events
  if (controller_->timeline_) {
    client_->SetTimeline(controller_->timeline_.get());
    ESP_LOGI(kLogTag, "Remote CW client wired to Timeline");
  }

  // Wire to KeyingSubsystem for PTT management
  if (controller_->keying_) {
    controller_->keying_->SetRemoteClient(client_.get(), cfg.ptt_tail_ms);
  }

  // Auto-start if configured
  if (config_.remote.auto_reconnect && config_.remote.server_host[0] != '\0') {
    esp_err_t err = client_->Start();
    if (err != ESP_OK) {
      ESP_LOGW(kLogTag, "Remote CW client start failed: %s (will retry)",
               esp_err_to_name(err));
    } else {
      ESP_LOGI(kLogTag, "Remote CW client started");
    }
  }

  return ESP_OK;
}
```

**File:** `components/app/application_controller.cpp`

**ApplicationController::Run() - Remove Tick():**

```cpp
void ApplicationController::Run() {
  ESP_LOGI(kLogTag, "Entering main loop");

  while (true) {
    int64_t loop_start_us = high_precision_clock_->GetCurrentTimeUs();

    // Keying subsystem tick (still needed)
    if (keying_subsystem_) {
      keying_subsystem_->Tick(loop_start_us);
    }

    // Audio subsystem tick
    if (audio_subsystem_) {
      audio_subsystem_->Tick(loop_start_us);
    }

    // REMOVED: remote_client_->Tick(loop_start_us);
    // Task is now autonomous!

    // Optional: Read state for UI updates (non-blocking atomic read)
    if (remote_client_) {
      auto state = remote_client_->GetState();
      auto latency = remote_client_->GetLatency();
      // Could update UI status here if needed
    }

    // ... rest of main loop (diagnostics, WiFi, etc.)

    // Loop timing
    int64_t loop_end_us = high_precision_clock_->GetCurrentTimeUs();
    int64_t elapsed_us = loop_end_us - loop_start_us;
    int64_t remaining_us = 1000 - elapsed_us;  // 1ms target
    if (remaining_us > 0) {
      vTaskDelay(pdMS_TO_TICKS(remaining_us / 1000));
    }
  }
}
```

### Timeline Hook Integration

**File:** `components/timeline/include/timeline/timeline_logger.hpp` (if not already present)

**Hook Registration API:**

```cpp
namespace timeline {

struct HookRegistration {
  void (*on_paddle_event)(bool key_down, int64_t timestamp_us, void* context);
  void* context;
};

class TimelineLogger {
 public:
  void RegisterHook(const HookRegistration& hook);
  void UnregisterHook(void* context);  // Unregister by context pointer

  // ... existing methods
};

}  // namespace timeline
```

**File:** `components/timeline/timeline_logger.cpp`

**Hook Invocation (when paddle event is logged):**

```cpp
void TimelineLogger::EmitPaddleEvent(bool key_down, int64_t timestamp_us) {
  // Log to ring buffer (existing code)
  LogEvent(EventType::kPaddleEvent, key_down, timestamp_us);

  // Invoke registered hooks (NEW)
  for (const auto& hook : hooks_) {
    if (hook.on_paddle_event) {
      hook.on_paddle_event(key_down, timestamp_us, hook.context);
    }
  }
}
```

**Note:** Timeline hooks pattern already documented in `docs/TIMELINE_HOOKS.md` - we're just using it!

---

## File Changes Summary

### New Files
None (all refactoring of existing files)

### Modified Files

1. **components/remote/include/remote/remote_cw_client.hpp** (~250 lines changed)
   - Add: `#include <atomic>`, `#include "freertos/FreeRTOS.h"`, `#include "freertos/task.h"`, `#include "freertos/queue.h"`
   - Add: `TaskHandle_t task_handle_`
   - Add: `QueueHandle_t paddle_queue_, cmd_queue_`
   - Change: `RemoteCwClientState state_` → `std::atomic<RemoteCwClientState> state_`
   - Add: `std::atomic<uint32_t> measured_latency_ms_`, `std::atomic<uint32_t> dropped_paddle_events_`
   - Add: `void SetTimeline(timeline::TimelineLogger*)` (public API)
   - Add: `void RunTaskLoop()` (private, called by task)
   - Add: `static void TaskFunction(void* param)` (task entry point)
   - Add: `static void OnPaddleEventHook(bool, int64_t, void*)` (Timeline callback)
   - Add: `void ProcessCommandQueue()`, `void HandleConnectedState()`, `void DrainPaddleQueue()`, etc.
   - **Remove: `void Tick(int64_t now_us)`** ← API BREAKING CHANGE!
   - Remove: `KeyingEvent key_queue_[kMaxKeyQueueDepth]` (replaced by FreeRTOS queue)

2. **components/remote/remote_cw_client.cpp** (~800 lines changed)
   - Implement task loop logic (`RunTaskLoop`, `TaskFunction`, etc.)
   - Implement `select()`-based I/O in `HandleConnectedState()`
   - Implement `ProcessCommandQueue()`, `DrainPaddleQueue()`
   - Convert `AttemptResolution()` to use blocking `getaddrinfo()` (OK in task!)
   - Remove all `Tick()` polling logic
   - Add Timeline hook callback implementation
   - Update `Configure()` to create task and queues
   - Update destructor to destroy task and queues

3. **components/app/init_phases.cpp** (~15 lines changed)
   - Line 485-486: Add early return if `!config_.remote.enabled` (FIX Bug #2)
   - Line 505-510: Add `client_->SetTimeline()` call (FIX Bug #3)

4. **components/app/application_controller.cpp** (~5 lines changed)
   - Remove `remote_client_->Tick(loop_start_us)` from main loop
   - Optional: Add atomic state reads for UI updates

5. **components/timeline/include/timeline/timeline_logger.hpp** (~20 lines added if not exists)
   - Add `HookRegistration` struct
   - Add `RegisterHook()`, `UnregisterHook()` methods
   - Storage: `std::vector<HookRegistration> hooks_` (or fixed array if avoiding STL)

6. **components/timeline/timeline_logger.cpp** (~30 lines added)
   - Implement `RegisterHook()`, `UnregisterHook()`
   - Invoke hooks in `EmitPaddleEvent()`

7. **.project-management/closed-prd/prd-remote-keying.md** (~5 lines changed)
   - Update TC-5: Remove "SHALL NOT create tasks" restriction
   - Add rationale: Performance and architecture benefits

---

## Testing Strategy

### Unit Tests (Host-based)

**File:** `tests_host/remote_cw_client_test.cpp`

**Challenges:** FreeRTOS primitives (`xTaskCreate`, `xQueueCreate`) require mocking.

**Approach:**
1. Test state machine logic in isolation (without task)
2. Test frame encoding/decoding (pure functions)
3. Test error handling (socket close, DNS failure paths)
4. Mock FreeRTOS primitives in `tests_host/stubs/freertos_stubs.cpp`

**Example Test:**
```cpp
TEST(RemoteCwClient, EncodeTimestampRanges) {
  EXPECT_EQ(RemoteCwClient::EncodeTimestamp(0), 0x00);
  EXPECT_EQ(RemoteCwClient::EncodeTimestamp(31), 0x1F);
  EXPECT_EQ(RemoteCwClient::EncodeTimestamp(32), 0x20);
  EXPECT_EQ(RemoteCwClient::EncodeTimestamp(156), 0x3F);
  EXPECT_EQ(RemoteCwClient::EncodeTimestamp(1165), 0x7F);
}
```

**Note:** Full integration test of task + queues requires ESP32 target (not host).

### Integration Tests (ESP32 Target)

**Manual Testing Checklist:**

1. **Initialization:**
   - [ ] `enabled=false` → Client not created (no task, no CPU usage)
   - [ ] `enabled=true, server_host=""` → Client created but not started
   - [ ] `enabled=true, auto_reconnect=true` → Client auto-starts

2. **Connection Lifecycle:**
   - [ ] DNS resolution successful
   - [ ] TCP connect successful
   - [ ] Handshake completes (CONNECT ACK received)
   - [ ] State transitions: Idle → Resolving → Connecting → Handshake → Connected

3. **Error Recovery:**
   - [ ] DNS failure → retry after 5 seconds
   - [ ] Server unreachable → retry after 5 seconds
   - [ ] Handshake timeout → retry
   - [ ] Server closes connection → reconnect

4. **Paddle Event Transmission:**
   - [ ] Paddle press → Timeline → Queue → Task → Server (verify with Wireshark)
   - [ ] Multiple rapid events → queue drains correctly
   - [ ] Queue overflow (stress test) → events dropped, counter incremented

5. **Performance:**
   - [ ] Main loop CPU usage < 10% (vs 42.7% before)
   - [ ] Task CPU usage < 1% when idle (select blocking)
   - [ ] Latency: Paddle event → network send < 50ms

6. **Stop/Start:**
   - [ ] `Stop()` → socket closed, state=Idle
   - [ ] `Start()` → reconnects successfully

### Performance Validation

**Before (baseline from bug report):**
```
Main loop: 110 µs / 1000 µs (11% busy)
├─ RemoteCw Tick: 47 µs (42.7% of loop)
└─ Other: 63 µs
```

**After (target):**
```
Main loop: 60-70 µs / 1000 µs (6-7% busy)
├─ RemoteCw: 0 µs (moved to task!)
└─ Other: 60-70 µs

RemoteCw Task: 0% when idle, <1% when active
```

**Measurement:** Use existing profiling code in main loop (already present per bug report).

---

## Migration Path

### Phase 1: PRD Update
- Update `.project-management/closed-prd/prd-remote-keying.md` TC-5
- Commit with message: `docs: Update PRD to allow RemoteCw FreeRTOS task`

### Phase 2: Timeline Hooks (if not exists)
- Add hook registration API to `timeline_logger.hpp/cpp`
- Test hook invocation with simple callback
- Commit: `feat(timeline): Add hook registration for paddle events`

### Phase 3: RemoteCwClient Refactor
- Implement task loop, queues, atomic state
- Remove `Tick()` method (breaking change)
- Commit: `refactor(remote): Convert RemoteCwClient to FreeRTOS task`

### Phase 4: ApplicationController Integration
- Fix init phase (Bug #2: early return)
- Wire Timeline hooks (Bug #3)
- Remove `Tick()` call from main loop
- Commit: `fix(app): Integrate task-based RemoteCwClient`

### Phase 5: Testing & Validation
- Run host tests (`./run_tests.sh`)
- Flash to ESP32, test connection lifecycle
- Measure performance (profiling)
- Commit: `test: Validate RemoteCwClient task performance`

### Phase 6: Documentation
- Update `docs/CHANGELOG.md`
- Update `docs/journal.md`
- Update `docs/REMOTE_KEYING.md` (if exists)
- Commit: `docs: Document RemoteCwClient task architecture`

---

## Risks & Mitigations

### Risk 1: Task Stack Overflow
**Likelihood:** Low
**Impact:** High (crash, watchdog reset)
**Mitigation:**
- Start with 4096 bytes stack (conservative)
- Monitor high-water mark: `uxTaskGetStackHighWaterMark(task_handle_)`
- Add diagnostic logging if stack usage > 75%

### Risk 2: Queue Overflow (Paddle Events)
**Likelihood:** Medium (if network latency spikes)
**Impact:** Low (dropped events, user sees gaps in remote keying)
**Mitigation:**
- Queue capacity 64 events (>7 seconds buffer)
- Atomic counter for dropped events (diagnostics)
- Rate-limited warnings (avoid log spam)

### Risk 3: DNS Blocking Too Long
**Likelihood:** Medium (slow/unreliable DNS servers)
**Impact:** Medium (task blocked for seconds)
**Mitigation:**
- Set DNS timeout in lwIP config (default ~5 seconds)
- Task blocked is OK (doesn't affect main loop)
- Consider caching resolved IP if hostname stable

### Risk 4: Memory Fragmentation (Task Creation)
**Likelihood:** Low (task created once at boot)
**Impact:** Low (4KB stack allocation)
**Mitigation:**
- Task created early in init phase (before heap fragmentation)
- Never destroy task (no dynamic create/destroy)

### Risk 5: Callback from ISR Context
**Likelihood:** High (paddle ISR → Timeline hook)
**Impact:** Critical if mishandled
**Mitigation:**
- Use `xQueueSendFromISR()` (ISR-safe)
- Use `portYIELD_FROM_ISR()` for context switch
- No logging in ISR (only in task)

---

## Future Enhancements (Out of Scope)

1. **RemoteCwServer Integration:** Apply same task pattern to server component
2. **Multiple Connections:** Support multiple remote clients in server mode
3. **Audio Streaming:** Extend task to handle CMD_AUDIO frames
4. **Reconnect Backoff:** Exponential backoff for retry delays
5. **Metrics:** Expose queue depth, dropped events, latency stats via Web UI
6. **IPv6 Support:** Extend DNS resolution to AF_INET6

---

## Conclusion

This design eliminates 42.7% of main loop CPU usage by moving RemoteCwClient to a dedicated FreeRTOS task with event-driven I/O. It fixes initialization bugs, integrates with Timeline system, and provides a clean separation of concerns.

**Key Benefits:**
- ✅ Zero CPU overhead when idle (`select()` blocking)
- ✅ Sub-millisecond socket response latency
- ✅ Timeline integration (no duplicate event storage)
- ✅ Robust error recovery
- ✅ Thread-safe atomic state access
- ✅ Scalable architecture (server can follow same pattern)

**Next Steps:** Implement Phase 1-6 migration path, validate performance targets.
