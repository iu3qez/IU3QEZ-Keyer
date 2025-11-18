# BUG REPORT: RemoteCwClient Architecture - Polling vs Event-Driven

**Date:** 2025-11-15
**Severity:** HIGH (Performance Regression + Architectural Violation)
**Component:** `components/remote/remote_cw_client.cpp`
**Discovered By:** Performance profiling (main loop CPU usage 30% → Remote Client 42.7%)

---

## Executive Summary

The RemoteCwClient implementation violates the original event-driven architecture defined in PRD Feature 2. Instead of emitting events to the Timeline system, it performs **blocking socket polling at 1000 Hz** from the main loop, consuming 47 µs per iteration (42.7% of main loop time).

**Root Cause:** Architectural drift between PRD Feature 2 (Timeline-based events) and PRD Remote Keying (polling model).

**Impact:**
- Main loop CPU usage increased from ~2% to 30%
- Reduced keying fluidity due to excessive CPU consumption
- `lwip_recv()` called 1000 times/second even when no data available

---

## Background: Original Architecture (PRD Feature 2)

### Timeline & Diagnostics (Original Design)

From `.project-management/closed-prd/prd-feature2.md` lines 104-108:

```markdown
### Timeline & Diagnostics
- Ring buffer (>=2048 events) capturing paddle edges, generated symbols,
  decoder output, spacing analysis.
- Export timeline snapshots via JSON/CSV over HTTP and CDC1 (reserved USB port).
- Provide optional statistical summary (average dit length, jitter, error counts).
- WS2812 RGB LED chain visually displays system status.
```

**Design Intent:** Timeline captures ALL events from ALL sources:
- ✅ Paddle edges (ISR → Timeline)
- ✅ Generated symbols (PaddleEngine → Timeline)
- ✅ Decoder output (MorseDecoder → Timeline)
- ❌ **Remote keying events (MISSING!)**

**Event-Driven Model:**
```
[Event Source] → Timeline Ring Buffer → [Consumers]
```

---

## Architecture Violation: PRD Remote Keying

From `.project-management/closed-prd/prd-remote-keying.md` line 275:

```markdown
**TC-2**: The client and server SHALL be polled from the main loop
(or subsystem tick) at regular intervals (≤50ms).
```

**Problem:** PRD Remote Keying does NOT reference the Timeline system!

**Implemented Architecture (WRONG):**
```
Main Loop (1000 Hz)
  ↓
RemoteCwClient::Tick()
  ↓
HandleSocketIo()
  ↓
lwip_recv() ← CALLED 1000 TIMES/SECOND!
```

**Expected Architecture (CORRECT):**
```
Socket Event (data available)
  ↓
RemoteCwClient (async handler)
  ↓
Timeline Ring Buffer → Emit remote key event
  ↓
Main Loop → Process Timeline events
```

---

## Bug #1: Excessive Socket Polling

### Symptoms

**Profiling Data (Main Loop):**
```
╠════════════════════════════════════════════════════════════════╣
║ Remote Client Tick:     47 µs  ( 42.7%)  ← PROBLEM!
║ TOTAL LOOP TIME:       110 µs / 1000 µs (11.0% busy)
```

**Analysis:**
- Main loop runs at 1000 Hz (1ms interval)
- `RemoteCwClient::Tick()` called every iteration
- `HandleSocketIo()` called every iteration when `state == kConnected`
- `lwip_recv()` called 1000 times/second

### Code Evidence

`components/remote/remote_cw_client.cpp:122-136`
```cpp
case RemoteCwClientState::kConnected:
  HandleSocketIo(now_us);  // ← CALLED EVERY 1ms!
  if (handshake_complete_) {
    // Check ping interval
    // Check PTT timeout
  }
  break;
```

`components/remote/remote_cw_client.cpp:312-344`
```cpp
void RemoteCwClient::HandleSocketIo(int64_t now_us) {
  // ...
  DrainTxBuffer();           // Every 1ms
  FlushKeyingQueue();        // Every 1ms

  // ⚠️ THIS IS THE PROBLEM!
  const ssize_t read_bytes =
    lwip_recv(socket_fd_, rx_buffer_ + rx_bytes_, available_space, 0);
  // ↑ CALLED 1000 TIMES/SECOND!
}
```

**Why This Is Expensive:**
- Even with `O_NONBLOCK`, `lwip_recv()` enters TCP stack
- Checks socket state, buffer availability
- Returns `EWOULDBLOCK` if no data
- **Overhead: ~40-50 µs per call** on ESP32-S3

**Calculation:**
- 1000 calls/sec × 47 µs/call = **47,000 µs/sec = 4.7% CPU**
- But measured 42.7% of loop time suggests additional overhead

---

## Bug #2: Initialization Logic Error

### Symptoms

User reported: "Remote CW Client is always active even when disabled"

### Code Evidence

`components/app/init_phases.cpp:495-498`
```cpp
// Check if remote client is enabled in config
if (!config_.remote.enabled) {
  ESP_LOGI(kLogTag, "Remote CW client disabled - instance available for runtime activation");
}
// ⚠️ NO RETURN! Continues to configure and potentially start client!
```

`components/app/init_phases.cpp:535-537`
```cpp
// Start connection if auto_reconnect is enabled
if (config_.remote.auto_reconnect) {
  esp_err_t err = client_->Start();  // ← Starts even if enabled=false!
```

**Logic Flaw:** `enabled=false` does NOT prevent `auto_reconnect=true` from starting client!

**Expected Behavior:**
```cpp
if (!config_.remote.enabled) {
  ESP_LOGI(kLogTag, "Remote CW client disabled");
  return ESP_OK;  // Don't configure or start
}
```

---

## Bug #3: Missing Timeline Integration

### Problem

RemoteCwClient does NOT consume events from Timeline system!

**Evidence:** Search for "Timeline" in `remote_cw_client.cpp`:
```bash
$ grep -i timeline components/remote/remote_cw_client.cpp
# No results!
```

**Expected Integration:**
```cpp
// When receiving CMD_MORSE from server:
void RemoteCwClient::HandleMorseFrame(...) {
  // Parse CW byte
  bool key_down = (cw_byte & 0x80) != 0;

  // ✅ SHOULD DO THIS:
  timeline_->EmitEvent(TimelineEvent::kRemoteKeyDown, timestamp_us);

  // Then invoke callback
  callbacks_.on_remote_key_event(key_down, timestamp_us, callbacks_.context);
}
```

**Current Implementation:** Callbacks only, no Timeline!

---

## Root Cause Analysis

### Design Drift Timeline

1. **PRD Feature 2 (2025-XX-XX):** Defines Timeline as central event system
2. **PRD Remote Keying (2025-10-28):** Does NOT reference Timeline!
3. **Implementation:** Polling model with no Timeline integration

### Why This Happened

**PRD Remote Keying TC-2:**
> "SHALL be polled from the main loop at regular intervals (≤50ms)"

**Misinterpretation:**
- Intended: "Check state transitions every ≤50ms"
- Implemented: "Call `Tick()` every 1ms and do socket I/O"

**Correct Interpretation:**
- State machine progression: Can poll at 1000 Hz (cheap)
- Socket I/O: Should be **event-driven** (select/poll/callback)

---

## Proposed Architecture: Event-Driven Socket I/O

### Option A: FreeRTOS Task + Event Queues

```
┌─────────────────────────────────────────────────┐
│ RemoteCwClientTask (separate FreeRTOS task)    │
│  - select() on socket_fd (blocking, event-driven)│
│  - When data available: recv() → parse frames  │
│  - Emit events to Timeline                     │
│  - Send events via FreeRTOS queue to main      │
└─────────────────────────────────────────────────┘
          ↓ (queue)
┌─────────────────────────────────────────────────┐
│ Main Loop                                       │
│  - Process timeline events                      │
│  - Update state based on queued messages        │
└─────────────────────────────────────────────────┘
```

**Pros:**
- True event-driven (no polling overhead)
- Matches original Timeline architecture
- `select()` blocks efficiently (0% CPU when idle)

**Cons:**
- Requires FreeRTOS task (violates TC-5: "SHALL NOT create additional tasks")
- More complex inter-task communication

---

### Option B: Throttled Polling with Timeline Integration

**Compromise:** Keep polling but reduce frequency + add Timeline

```cpp
void RemoteCwClient::Tick(int64_t now_us) {
  switch (state_) {
    case RemoteCwClientState::kConnected:
      // Throttle socket I/O to 50 Hz (20ms interval)
      if (now_us - last_socket_io_us_ >= 20000) {  // 20ms
        HandleSocketIo(now_us);
        last_socket_io_us_ = now_us;
      }

      // Lightweight state checks can run at 1000 Hz
      if (handshake_complete_) {
        CheckPingInterval(now_us);
        CheckPttTimeout(now_us);
      }
      break;
  }
}
```

**Emit to Timeline:**
```cpp
void RemoteCwClient::HandleMorseFrame(...) {
  // Parse event
  bool key_down = (cw_byte & 0x80) != 0;

  // Emit to Timeline (NEW!)
  if (timeline_) {
    timeline::TimelineEvent event;
    event.type = key_down ? timeline::EventType::kRemoteKeyDown
                          : timeline::EventType::kRemoteKeyUp;
    event.timestamp_us = last_remote_key_timestamp_us_;
    timeline_->EmitEvent(event);
  }

  // Also invoke callback for immediate handling
  callbacks_.on_remote_key_event(key_down, timestamp_us, context);
}
```

**Pros:**
- Maintains polling model (matches TC-5)
- Reduces CPU from 47 µs → ~5 µs (90% reduction)
- Adds Timeline integration (fixes architecture violation)

**Cons:**
- Still not true event-driven
- 20ms latency for socket I/O (acceptable for CW ping/keying)

---

### Option C: lwIP Callback API (Netconn)

Use lwIP's callback-based netconn API instead of sockets:

```cpp
// Callback invoked when data arrives (event-driven!)
static void tcp_recv_callback(void *arg, struct tcp_pcb *tpcb,
                              struct pbuf *p, err_t err) {
  RemoteCwClient* client = static_cast<RemoteCwClient*>(arg);

  // Process received data
  client->ProcessIncomingData(p);

  // Emit to Timeline
  client->EmitTimelineEvent(...);

  pbuf_free(p);
}
```

**Pros:**
- True event-driven (0% CPU when idle)
- No FreeRTOS task required
- Minimal code changes

**Cons:**
- Callback runs in lwIP context (TCPIP thread)
- Requires thread-safe Timeline access

---

## Recommended Fix: Option B (Throttled + Timeline)

### Rationale

1. **Minimal code change** (most practical for immediate fix)
2. **Preserves polling model** (matches PRD TC-5)
3. **Reduces CPU 90%** (47 µs → 5 µs)
4. **Adds Timeline integration** (fixes architecture violation)
5. **20ms socket I/O latency acceptable** (CW keying is ~40ms per dit at 30 WPM)

### Implementation Steps

#### Step 1: Add Throttling Variable
```cpp
// components/remote/include/remote/remote_cw_client.hpp
private:
  int64_t last_socket_io_us_ = 0;  // Throttle socket I/O
```

#### Step 2: Throttle HandleSocketIo()
```cpp
// components/remote/remote_cw_client.cpp
case RemoteCwClientState::kConnected:
  // Socket I/O throttled to 50 Hz (20ms interval)
  if (now_us - last_socket_io_us_ >= 20000) {
    HandleSocketIo(now_us);
    last_socket_io_us_ = now_us;
  }

  // Lightweight checks run at 1000 Hz
  if (handshake_complete_) {
    if (now_us - last_ping_time_us_ >= ...) SendPingRequest(now_us);
    if (ptt_active_ && ...) SendPttCommand(false);
  }
  break;
```

#### Step 3: Add Timeline Integration
```cpp
// Add Timeline dependency
void RemoteCwClient::SetTimeline(timeline::TimelineLogger* timeline) {
  timeline_ = timeline;
}

// Emit events in HandleMorseFrame
void RemoteCwClient::HandleMorseFrame(...) {
  // ...parse CW byte...

  // Emit to Timeline
  if (timeline_) {
    timeline_->EmitEvent(
      key_down ? timeline::EventType::kRemoteKeyDown
               : timeline::EventType::kRemoteKeyUp,
      last_remote_key_timestamp_us_
    );
  }

  // Callback for immediate handling
  callbacks_.on_remote_key_event(...);
}
```

#### Step 4: Wire Timeline in Initialization
```cpp
// components/app/init_phases.cpp - RemoteClientPhase
client_->SetTimeline(&controller_->keying_subsystem_->GetTimeline());
```

#### Step 5: Fix Initialization Logic
```cpp
// components/app/init_phases.cpp:495
if (!config_.remote.enabled) {
  ESP_LOGI(kLogTag, "Remote CW client disabled");
  return ESP_OK;  // Early exit - don't configure or start
}
```

---

## Expected Results After Fix

### Performance
```
Before:
║ Remote Client Tick:     47 µs  ( 42.7%)

After (throttled to 50 Hz):
║ Remote Client Tick:      5 µs  (  4.5%)
```

**CPU Reduction:** 42.7% → 4.5% (90% improvement)

### Architecture
- ✅ Socket I/O reduced from 1000 Hz to 50 Hz
- ✅ Timeline integration added (events visible in diagnostics)
- ✅ Initialization respects `enabled` flag
- ✅ Main loop fluidity restored

---

## Long-Term Recommendation: Full Event-Driven Refactor

For future phases, consider **Option A (FreeRTOS Task)**:

1. Create `RemoteCwClientTask` with dedicated socket handling
2. Use `select()` for true event-driven I/O (0% CPU when idle)
3. Communicate with main loop via FreeRTOS queues
4. Full Timeline integration

**Benefits:**
- Zero CPU when idle
- Sub-millisecond socket latency
- Matches original Timeline architecture
- Scalable for future features (audio streaming, multi-client)

**Tradeoffs:**
- Violates current TC-5 constraint
- Requires PRD update to allow FreeRTOS tasks
- More complex implementation

---

## Related Issues

1. **DiagnosticsSubsystem also high:** 31 µs (28.2%) - investigate LED rendering frequency
2. **No remote events in Timeline export:** Web UI timeline missing remote keying data
3. **DNS blocking:** `getaddrinfo()` in kResolving state may block (even with IP address)

---

## References

- **PRD Feature 2:** `.project-management/closed-prd/prd-feature2.md`
- **PRD Remote Keying:** `.project-management/closed-prd/prd-remote-keying.md`
- **Profiling Data:** Main loop analysis (2025-11-15)
- **Timeline Architecture:** `docs/TIMELINE_HOOKS.md`

---

**Status:** OPEN
**Assignee:** Development Team
**Target Fix:** v1.1 (Throttling + Timeline Integration)
**Target Refactor:** v2.0 (Full Event-Driven Architecture)
