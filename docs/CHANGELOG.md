# Keyer QRS2HST - Changelog

**Archived**: Previous entries moved to [CHANGELOG.md.1](CHANGELOG.md.1)

---

## 2025-11-17

2025-11-17 - Implemented preset customization system (Task 1.0 + 2.0 + 3.0 + migration v4→v5)
  - Extended PresetConfigData struct from 5 to 8 fields (added timing_l/s/p)
  - Added preset_definitions[10] array to KeyingConfig for per-preset customization
  - Implemented SavePresets/LoadPresets for NVS blob storage (~640 bytes)
  - Implemented SaveManualLSP/LoadManualLSP for Manual mode persistence
  - Modified GetPresetConfig() with user customization fallback logic
  - Added GetActivePresetConfig() helper for simplified runtime access
  - Implemented config migration v4→v5 (migrates global L-S-P to V9 + Manual mode)
  - Anti-bootloop design: graceful fallback to factory defaults on NVS errors

## 2025-11-16

2025-11-16 10:14 - Fixed CPU usage percentages on dual-core ESP32-S3 in system monitor
  - Problem 1: Percentages exceeded 100% (used real time instead of CPU time as denominator)
  - Problem 2: Percentages were halved (IDLE0/IDLE1 showed ~50% instead of ~100% when idle)
  - Root cause: total_runtime from uxTaskGetSystemState() is real time, not sum of task runtimes
  - Fix: Sum all task runtimes manually, then scale by number of cores (portNUM_PROCESSORS)
  - Result: Each core can now show up to 100% utilization (total max = 200% on dual-core)

## 2025-11-15

2025-11-15 20:45 - Added TAB autocomplete for console debug and reboot commands
  - Debug command: autocomplete for log levels, tags, and special commands (tags, timeline)
  - Reboot command: added "reboot bootloader" subcommand with autocomplete
  - Extended Command structure with TabCompleteHandler for argument completion
  - Modified InputHandler::handleTab() to support multi-argument autocomplete

### Fix: RemoteCwClient kStart Command Being Discarded (2025-11-15 14:30)

**Critical bug fix in WaitForCommandOrTimeout() that was discarding kStart commands.**

**Root Cause:** Line 273 checked `if (cmd.cmd != static_cast<TaskCommand>(0))` which evaluates to "if NOT kStart". Since `TaskCommand::kStart = 0`, this condition was:
- kStart (value 0): FALSE → command **discarded** (BUG!)
- kStop (value 1): TRUE → command re-queued correctly
- kUpdateConfig (value 2): TRUE → command re-queued correctly

**Symptoms:** User reported `remote start` command had no effect - no connection attempts, no state transitions, no log messages. Only `remote stop` worked.

**Fix:** Changed condition to check `xQueueReceive` return value instead of command content. Now all commands are properly re-queued for ProcessCommandQueue to handle.

**Commit:** 8a59f95 - `fix(remote): Fix WaitForCommandOrTimeout discarding kStart commands`

---

### Fix: RemoteCwClient CONNECT Frame Send Failure (2025-11-15 15:45)

**Critical bug fix in EnterHandshake() that prevented CONNECT frame from being sent to server.**

**Root Cause:** `DrainTxBuffer()` called immediately after `connect()` in `EnterHandshake()` failed because socket not ready for writes. Non-blocking socket returns `EWOULDBLOCK`, leaving CONNECT frame (94 bytes) buffered but unsent. Server never receives handshake, waits 3 seconds, then closes connection.

**Symptoms (confirmed by user logs with commit 6c1ff72 diagnostic logging):**
- `EnterHandshake: CONNECT frame NOT fully sent! 94 bytes remaining in buffer`
- `Handshake timeout` after 3 seconds (config.handshake_timeout_ms)
- `Server closed connection (recv returned 0)` with `rx_bytes=0` (no data ever received from server)

**Root Cause Analysis:** Before refactoring, `Tick()` was called repeatedly from main loop, retrying send until successful. Task-based architecture requires event-driven approach - socket must use `select()` to wait for writability before attempting send.

**Fix:** Removed `DrainTxBuffer()` call from `EnterHandshake()`. Now `HandleConnectedState()` uses `select()` with `FD_SET(socket_fd_, &writefds)` when `tx_head_ != tx_tail_` (data buffered). When `select()` indicates socket ready, `HandleSocketWrite()` drains buffer with guaranteed success.

**Flow After Fix:**
1. `EnterHandshake()` → `PopulateConnectFrame()` (94 bytes buffered, tx_head=0, tx_tail=94)
2. `HandleConnectedState()` → `select()` waits for socket writability
3. When ready → `HandleSocketWrite()` → `DrainTxBuffer()` → CONNECT frame sent successfully
4. Server receives complete CONNECT frame → sends ACK → handshake completes

**Commit:** 1867ceb - `fix(remote): Remove DrainTxBuffer from EnterHandshake to prevent send failure`

---

### Refactor: RemoteCwClient Event-Driven Architecture with FreeRTOS Task

**Refactored RemoteCwClient from polling-based to event-driven architecture using dedicated FreeRTOS task to eliminate 42.7% CPU overhead.**

**Performance Impact:**
- **Before**: RemoteCwClient consumed 47 µs per main loop iteration (42.7% of 110 µs total loop time)
- **After**: Zero main loop overhead (task runs independently with `select()` blocking I/O)
- **Expected**: Main loop CPU usage drops from 11% to <1% when remote client active

**Implementation:**
- `docs/plans/2025-11-15-remotecw-task-architecture-design.md`: Complete design document (923 lines)
- **Breaking Changes**:
  - Removed `Tick(int64_t now_us)` method (no longer called from main loop)
  - Renamed `QueueLocalKeyEvent()` → `QueueKeyingEvent()` (clearer naming)
  - Renamed `state()` → `GetState()` (atomic read with relaxed memory order)
  - Renamed `latency_ms()` → `GetLatency()` (atomic read)
  - Added `GetDroppedEventCount()` for diagnostic monitoring
- **Architecture**: Dedicated FreeRTOS task (priority: tskIDLE_PRIORITY + 2, stack: 4KB)
  - Event-driven I/O with `select()` (50ms timeout for periodic tasks)
  - Thread-safe state management with `std::atomic<RemoteCwClientState>`
  - ISR-safe keying event queue (capacity: 64 events, ~7s buffer at 9 events/sec)
  - Command queue for Start/Stop/UpdateConfig (capacity: 4 commands)
- **PRD Update**: TC-5 constraint changed from "SHALL NOT create tasks" to "SHALL use dedicated task"
  - Rationale: Performance profiling showed 42.7% CPU usage unacceptable for critical component
- **Timeline Integration**: Added `OnKeyingChanged()` hook to `timeline::TimelineHooks`
  - Called by KeyingSubsystem when CW output changes state
  - Consumed by RemoteCwClient via FreeRTOS queue in task context

**Files Modified:**
- `components/remote/include/remote/remote_cw_client.hpp`: Added task management, queues, atomics
- `components/remote/remote_cw_client.cpp`: Implemented event-driven task loop, removed old polling code
- `components/timeline/include/timeline/timeline_hooks.hpp`: Added `OnKeyingChanged()` hook
- `components/keying_subsystem/keying_subsystem.cpp`: Updated to call `QueueKeyingEvent()` and `GetState()`
- `components/app/application_controller.cpp`: Removed `remote_client_->Tick()` call and profiling infrastructure
- `.project-management/closed-prd/prd-remote-keying.md`: Updated TC-5 constraint with rationale

**Testing**: Code complete, ready for compilation test and validation on hardware.

---

## 2025-11-15

### Feature: WiFi Captive Portal for Access Point Mode (In Progress)

**Planning Phase (Complete):**
- `.project-management/current-prd/prd-captive-portal.md`: Complete PRD with captive portal requirements
- `.project-management/current-prd/tasks-prd-captive-portal.md`: 96 sub-tasks across 7 parent tasks
- `docs/WIFI_EXPLORATION_REPORT.md`: WiFi subsystem analysis (STA+AP modes, configuration, HTTP server infrastructure)

**Implementation Phase (Task 1.0 Complete - 16/16 sub-tasks):**

**11:30** - Captive portal component foundation implemented
- Created `components/captive_portal/` with isolated architecture (no dependency on components/ui)
- DNS server (287 lines): UDP port 53, wildcard resolution, rate limiting (100 queries/sec)
- Minimal HTTP server (344 lines): 11 endpoints for captive portal detection + WiFi APIs
- Captive portal manager: Lifecycle coordination for DNS + HTTP servers
- Setup page stub: Hardcoded HTML placeholder

**11:45** - HTTP captive portal detection endpoints complete
- Apple iOS/macOS: `/hotspot-detect.html`, `/library/test/success.html`
- Android: `/generate_204`, `/gen_204` (HTTP 204 No Content)
- Windows: `/connecttest.txt`, `/ncsi.txt`
- Firefox: `/success.txt`
- Wildcard redirect: `/*` → HTTP 302 to `/setup`

**12:00** - Unit tests and ESP-IDF stubs added
- `tests_host/dns_server_test.cpp`: 10 test cases (currently disabled, awaiting full lwip stubbing)
- ESP-IDF stubs: lwip socket API, esp_http_server API, esp_restart
- Stubs allow compilation but defer full implementation testing

**Architecture Decision:**
- Dedicated minimal HTTP server (NOT reusing components/ui)
- Rationale: Captive portal used once, must be lightweight (~25-30KB vs 168KB Web UI)
- Zero conflicts: HTTP servers never coexist (AP mode vs STA mode)

## 2025-11-16

**Implementation Phase (Task 2.0 Complete - 11/11 sub-tasks):**

**10:15** - WiFi scanning API and client monitoring implemented
- `components/wifi_subsystem/wifi_subsystem.hpp`: Added WifiScanResult struct, ScanNetworks() and GetConnectedClientCount() methods
- `components/wifi_subsystem/wifi_subsystem.cpp`: Complete ScanNetworks() implementation (104 lines)
  - Active WiFi scan with configurable timeout
  - Duplicate SSID filtering (keeps strongest RSSI)
  - 5GHz network filtering (device is 2.4GHz only)
  - RSSI descending sort (strongest first)
  - Concurrent scan prevention with atomic flag
  - Comprehensive ESP_LOG* logging
- Periodic client count monitoring in Tick() method (polls every 5 seconds in AP mode)
- ESP-IDF stubs: esp_wifi_scan_start(), esp_wifi_scan_get_ap_records(), esp_wifi_ap_get_sta_list()

**Implementation Phase (Task 4.0 Complete - 17/17 sub-tasks):**

**11:30** - WiFi setup page UI complete (production-ready)
- `components/captive_portal/setup_page.cpp`: Complete rewrite from stub (750 lines total)
- **HTML Structure**: Header, network list container, manual entry form, action buttons
- **CSS Features** (414 lines):
  - Mobile-first responsive design (320px-1920px breakpoints)
  - Purple gradient background (#667eea → #764ba2)
  - Blue primary theme (#1976d2) with WCAG 2.1 AA contrast
  - Network cards with hover/selection states
  - Signal strength bars (4-level visualization)
  - Loading spinners, status messages (success/error/info)
  - Smooth animations (slideIn, spin, transitions)
- **JavaScript Features** (256 lines):
  - `scanNetworks()`: Auto-scan on page load, fetch from /api/wifi/scan
  - `displayNetworkList()`: Render network cards with RSSI bars (>-50=4, >-60=3, >-70=2, >-80=1)
  - `selectNetwork()`: Auto-fill SSID, focus password field for secured networks
  - `togglePassword()`: Show/hide password with eye icon
  - `validateForm()`: Real-time validation (SSID 1-32 chars, password 0-63 chars)
  - `submitConfig()`: POST to /api/wifi/configure, countdown timer for reboot
  - Error handling with user-friendly messages
- **User Experience**:
  - Lock icon (🔒) for secured networks
  - Empty state messages ("No networks found", "Scan failed")
  - Disabled button states during API calls
  - Countdown timer: "Rebooting in N seconds..."

**Implementation Phase (Task 5.0 Complete - 12/12 sub-tasks):**

**13:00** - WiFi configuration workflow complete (backend API)
- `components/captive_portal/CMakeLists.txt`: Added cJSON dependency (json component)
- `components/captive_portal/minimal_http_server.cpp`: Complete API implementation
- **GET /api/wifi/scan**:
  - Calls WiFiSubsystem::ScanNetworks() (max 20 networks)
  - cJSON serialization: `{"networks": [...], "scan_time_ms": N}`
  - Authmode enum → string conversion (OPEN, WPA2-PSK, WPA3-PSK, etc.)
  - Scan timing measurement with esp_timer_get_time()
  - Error handling with JSON error responses
- **POST /api/wifi/configure**:
  - cJSON body parsing (sta_ssid, sta_password fields)
  - Parameter validation: SSID 1-32 chars (required), password 0-63 chars (optional)
  - DeviceConfig update with strncpy + null termination
  - NVS persistence via ConfigStorage::SaveToNvs()
  - FreeRTOS software timer for 2-second delayed reboot
  - Comprehensive error handling (invalid JSON, validation failures, NVS errors)
  - JSON success response: `{"status": "success", "message": "...", "reboot_delay_ms": 2000}`
  - Detailed ESP_LOG* logging for all operations
- **RebootTimerCallback**: FreeRTOS timer callback → esp_restart()

**14:30** - Captive portal integration into ApplicationController
- Added CaptivePortalPhase (Phase 15.5) to initialization pipeline
- Dynamic enable/disable based on WiFi mode and client count in main loop
- HTTP server port 80 coordination between main HTTP server and captive portal
  - CaptivePortalManager::Enable() stops main HTTP server before starting captive portal
  - CaptivePortalManager::Disable() restarts main HTTP server after stopping captive portal
- Fixed compilation errors: missing <cstddef>, config::Storage class name, Save() method name, raw string literal delimiter
- DNS server verified working by user testing
- Main HTTP server coordination implemented to prevent port conflict

**15:45** - LED rainbow pattern for captive portal setup mode
- `components/diagnostics_subsystem/diagnostics_subsystem.hpp`: Added SetRainbowPattern() and StopRainbowPattern() methods
- `components/diagnostics_subsystem/diagnostics_subsystem.cpp`: Complete rainbow animation implementation
  - UpdateRainbowAnimation(): Smooth HSV hue cycling (0-360° over 3 seconds)
  - HsvToRgb(): Standard HSV to RGB conversion algorithm (6-sector approach)
  - Rainbow pattern has highest priority in Tick() rendering chain (overrides paddle/WiFi animations)
  - Brightness: 50% (kRainbowBrightness = 128/255)
  - Saturation: 100% (vivid colors across spectrum)
  - Cycle duration: 3 seconds (kRainbowCycleDurationUs = 3,000,000 µs)
  - ESP_LOGI logging for pattern start/stop events
- ApplicationController integration:
  - SetRainbowPattern() called when captive portal enables (AP mode + client connected)
  - StopRainbowPattern() called when captive portal disables (STA mode or no clients)
  - Visual feedback: User sees rainbow cycling → knows device is in setup mode
- Additional patterns (scanning, saved, rebooting) deferred for future implementation

**17:30** - Simplified captive portal to manual SSID entry only (WiFi scan removed)
- Decision: Removed WiFi scanning functionality due to reliability issues and complexity
- WiFi scan attempts (16:20, 17:00) revealed APSTA mode switching complexity:
  - Performance issues (33% WiFi task CPU usage)
  - Event handler conflicts (auto-connect interfering with scan)
  - Scan finding 0 networks despite fixes
- Solution: Manual SSID/password entry only (simpler, more reliable)
- Changes:
  - ScanNetworks() stubbed (returns empty list immediately)
  - Event handler restored to simple STA_START auto-connect
  - setup_page.cpp completely rewritten (750 lines → 408 lines, -46%)
  - Removed network list UI, scan button, auto-scan JavaScript
  - Clean manual entry form with validation and password toggle
- Benefits:
  - No APSTA mode switching (WiFi task ~5% CPU)
  - No scan delays or reliability issues
  - Simpler user experience (2 fields + button)
  - Faster page load
- User workflow: Enter SSID + password manually → Save & Connect → device reboots

**18:00** - Fixed high CPU usage in WiFi and main tasks (throttling)
- Issue: High CPU usage in WiFi task and main task even after scan removal
- Root causes identified:
  1. DNS server polling 100 times/second (10ms delay when idle)
  2. Main loop checking captive portal 1000 times/second (every 1ms iteration)
  3. GetConnectedClientCount() called every 1ms (expensive ESP-IDF call)
- Solutions:
  1. DNS server: Increased idle delay from 10ms → 100ms (10 polls/sec, 90% reduction)
  2. Main loop: Added 500ms throttling for captive portal checks (2 checks/sec, 99.95% reduction)
- Result: WiFi and main task CPU usage reduced to normal levels
- User impact: None (500ms delay imperceptible for client detection)

**Remaining Tasks:**
1. ~~Create Captive Portal Component (16 sub-tasks)~~ ✅ COMPLETE
2. ~~Implement WiFi Network Scanning API (11 sub-tasks)~~ ❌ REMOVED (manual entry only)
3. ~~Add HTTP Captive Portal Detection Endpoints (12 sub-tasks)~~ ✅ COMPLETE
4. ~~Create WiFi Setup Page UI (17 sub-tasks)~~ ✅ COMPLETE
5. ~~Implement WiFi Configuration Workflow (12 sub-tasks)~~ ✅ COMPLETE
6. ~~Add LED Rainbow Pattern for Setup Mode (7/10 sub-tasks)~~ ✅ CORE COMPLETE (optional patterns deferred)
7. Integration and Cross-Platform Testing (18 sub-tasks) - PENDING

**Progress:** 75/96 sub-tasks complete (78%)

---

## 2025-11-06

### Feature: USB Firmware Updates via UF2 Bootloader (tinyuf2)

**Implemented build system and infrastructure for USB-based firmware updates using drag-and-drop UF2 files.**

**Deliverables:**
- `.project-management/current-prd/prd-ota-uf2-bootloader.md`: Complete PRD with UF2 bootloader integration requirements
- `.project-management/current-prd/tasks-prd-ota-uf2-bootloader.md`: 83 sub-tasks across 6 parent tasks
- `bootloader/tinyuf2/`: tinyuf2 bootloader submodule (Adafruit, ESP32-S3 support)
- `scripts/build/uf2conv.py`: Microsoft UF2 converter utility
- `scripts/build/generate_uf2.sh`: Wrapper script for ESP32-S3 firmware conversion
- Updated `partitions.csv`: Added 1MB factory partition for bootloader

**Implementation Highlights (Tasks 1.1-3.15, 4.1-4.14, 6.1-6.8 completed - 48/83 tasks):**

**Bootloader Integration (Group 1):**
- tinyuf2 added as git submodule (commit 45d535e, v0.35.0-13)
- Custom ESP32-S3 board config: `bootloader/tinyuf2/ports/espressif/boards/keyer_qrs2hst/`
- USB VID/PID: 0x303A:0x8002 (Espressif generic)
- Virtual drive label: "KEYERBOOT"
- LED: GPIO21 (shared with diagnostics subsystem)
- Virtual drive files: README.TXT, FACTORY_RESET.TXT
- Factory partition flash helper: `scripts/build/flash_factory.sh`

**Partition Table Changes:**
- Added `factory` partition: 1MB at 0x34000 (for UF2 bootloader)
- Reduced SPIFFS: 7920KB → 6896KB (-1024KB)
- New layout: nvs (96KB) + otadata (8KB) + phy_init (4KB) + coredump (64KB) + **factory (1MB)** + ota_0 (4MB) + ota_1 (4MB) + spiffs (6896KB)
- Total: 15.8MB / 16MB (99% utilization)

**Build System (Group 2):**
- UF2 Family ID: 0xc47e5767 (ESP32-S3, verified from Microsoft uf2families.json)
- Base address: 0x134000 (ota_0 partition with factory bootloader)
- CMake target: `ninja -C build uf2` generates `build/firmware.uf2`
- uf2conv.py: Microsoft reference implementation
- generate_uf2.sh: ESP32-S3 wrapper script

**Bootloader Entry Mechanisms (Group 3):**
- RTC memory magic value: 0xB007ACED (persists across soft resets)
- BootloaderCheckPhase: Phase 0 (before all other init phases)
- Console command: `upgrade` - enter bootloader with confirmation message
- Web UI: `/firmware` page with "Enter Bootloader Mode" button
- HTTP API: POST `/api/enter-bootloader` endpoint
- Integration: bootloader_entry.cpp/hpp module, Phase 0 in init pipeline

**Boot Failure Tracking & NVS Recovery (Group 4):**
- RTC-based failure counter (3 consecutive failures → safe mode)
- BootFailureCheckPhase: Phase 0.5 (after bootloader check, before NVS init)
- Automatic bootloop detection: enters bootloader for recovery
- Manual factory reset: console command `factory-reset confirm` (already existed)
- NVS erase trigger: delete FACTORY_RESET.TXT from bootloader drive
- Counter clears on successful boot (18 init phases complete)

**Documentation (Group 6 - partial):**
- `docs/FIRMWARE_UPDATE.md`: Complete user guide (USB updates, troubleshooting, building from source)
- `docs/NVS_RECOVERY.md`: Bootloop protection, recovery procedures, NVS details
- Updated `docs/DEVELOPMENT.md`: UF2 generation instructions
- Updated `docs/CHANGELOG.md`: This entry

**Remaining Work (Tasks 1.4-1.12, 2.7, 3.16-3.17, 4.15-4.17, 5.1-5.11, 6.2-6.5, 6.9-6.15):**
- Group 1: TinyUSB MSC callbacks, UF2 write handler, checksum validation (tinyuf2 bootloader code)
- Group 2: Hardware testing of UF2 generation
- Group 3: Hardware testing of bootloader entry (console and web UI)
- Group 4: Hardware testing of bootloop recovery
- Group 5: LED signaling implementation (orange pulsing, red blinking patterns)
- Group 6: Screenshots/diagrams for documentation, final documentation review

**References:**
- tinyuf2: https://github.com/adafruit/tinyuf2
- UF2 specification: https://github.com/microsoft/uf2
- ESP32-S3 TinyUSB: Already configured (2x CDC ports in sdkconfig.defaults)
## 2025-11-04

### PRD Closure - Three Major Features Complete

**Closed PRDs (moved to `.project-management/closed-prd/`):**
- `prd-feature2.md` (Main Keyer System) - Core functionality complete
- `prd-realtime-timeline.md` (Timeline Visualization) - Frontend + backend working
- `prd-paddle-engine-rewrite.md` (4-State FSM) - 96% complete (48/50 tasks)

**Feature Summary:**
- ✅ **Audio Subsystem**: ES8311 codec, sidetone with ADSR envelopes, configurable frequency/volume
- ✅ **Keying System**: PaddleEngine 4-state FSM (IDLE/SEND_DIT/SEND_DAH/GAP), 10 iambic presets (V0-V9)
- ✅ **Configuration**: Parameter metadata system, NVS persistence, Web UI with dynamic forms
- ✅ **Timeline Visualization**: 878-line JavaScript frontend, REST API backend, real-time canvas rendering
- ✅ **Morse Decoder**: Adaptive timing classifier, 99%+ accuracy at 60 WPM, character/word gap detection

**Code Cleanup:**
- Removed dead code from `paddle_engine.cpp` (547 → 532 lines, -15 lines)
  - Deleted obsolete methods: `ArmMemory()`, `ShouldHoldRepeat()`, `MemoryEnabledFor()`
  - Logic migrated to new FSM methods: `CheckMemoryAndSqueezeDuringElement()`
- Removed declarations from `paddle_engine.hpp` (3 function signatures)

**Follow-Up Tasks:**
- See `.project-management/follow-up-tasks.md` for non-blocking polish items
- Manual hardware testing (timeline visualization, paddle FSM behavior)
- Unit test creation for `TimelineEventEmitter` (Task 5.13)

**Build Status**: Firmware compiles successfully (1.31 MB, 68% partition free), 78/82 unit tests passing

---

## 2025-11-02

### Project Planning: PRD and Task List for Paddle Engine Complete Rewrite

**Created comprehensive PRD and 40+ task breakdown for paddle_engine rewrite based on Python FSM prototype.**

**Deliverables:**
- `.project-management/current-prd/prd-paddle-engine-rewrite.md`: Complete PRD with 27 functional requirements
- `.project-management/current-prd/tasks-prd-paddle-engine-rewrite.md`: Detailed task list with 5 parent tasks and specific sub-tasks

**PRD Highlights:**
- FSM model based on `docs/state_machine.py` (4 states: IDLE, SEND_DIT, SEND_DAH, INTER_ELEMENT_GAP)
- Remove `LateReleaseBehavior::kForget` codebase-wide (user-requested simplification)
- Rename `LatchMode` → `SqueezeMode` (LIVE/SNAPSHOT for Mode B bonus timing)
- New config parameters: `iambic_mode`, `memory_mode`, `squeeze_mode`, `mem_block_start_pct`, `mem_block_end_pct`
- Target: <400 LOC (down from ~555), clear mapping to Python prototype

**Task Breakdown:**
- 1.0: Define new data structures, remove deprecated code (8 sub-tasks)
- 2.0: Implement core FSM (10 sub-tasks mapping directly to Python `tick()` method)
- 3.0: Implement memory logic with windows (8 sub-tasks)
- 4.0: Implement Mode B bonus and squeeze detection (7 sub-tasks)
- 5.0: Wire up callbacks, stubs, testing (10 sub-tasks including manual validation)

Each sub-task references specific PRD requirements and Python prototype line numbers for traceability.

---

### Bugfix: Timeline Missing Paddle and TX Output Events

**Fixed critical visualization bug where paddle events (blue/red) and TX output (green) bars were not visible on timeline canvas.**

**Problem:**
- Paddle press/release events rendered as invisible 2px ticks (release events drawn white on white background)
- No duration-based bar rendering (each event independent instead of press→release pairing)
- TX keying output similarly invisible

**Solution:**
- Implemented press/release event pairing state machine in timeline.js
- Created `drawPaddleTrack()` method: filters events by paddle type (dit/dah), pairs press (arg1=1) → release (arg1=0), draws proportional-width bars
- Created `drawKeyingTrack()` method: pairs TX active → inactive events with same pairing logic
- Duration-proportional rendering: bar width = `timestampRelease - timestampPress` with 2px minimum
- Open events: press without release extends bar to canvas right edge (paddle held down)

**User-visible changes:**
- Timeline now correctly displays blue bars (dit paddle), red bars (dah paddle), green bars (TX output)
- Bar width shows actual duration of paddle press or TX output
- Real-time visualization matches physical paddle/keying timing

**Files modified:**
- webui/js/timeline.js (+130 lines: 2 new pairing methods, -65 lines old tick rendering, net +65)

---

## 2025-11-01

### Timeline Visualization - Frontend Complete + Backend Integration Started (Groups 1.0-4.0 Complete, 5.0 Partial)

**Implemented complete real-time timeline visualization with HTML5 Canvas frontend and backend hook integration:**

**Group 1.0 - Timeline Hook Architecture (Tasks 1.1-1.5):**
- Created timeline hook system following PaddleEngineCallbacks pattern (zero code duplication)
- TimelineHooks struct with 4 callbacks: OnMemoryWindowChanged, OnLateReleaseActive, OnLatchStateChanged, OnSqueezeDetected
- TimelineEventEmitter class with static callback handlers and context pointer pattern
- Comprehensive documentation in [docs/TIMELINE_HOOKS.md](docs/TIMELINE_HOOKS.md) with integration examples

**Group 2.0 - EventType Extension and REST API (Tasks 2.1-2.6):**
- Extended EventType enum from 5 to 11 types (backward compatible): kMemoryWindow, kLateRelease, kLatch, kSqueeze, kGapMarker, kDecodedChar
- Implemented REST API endpoints: GET `/api/timeline/events` (event streaming with since/limit params), GET `/api/timeline/config` (WPM source detection)
- EventType to JSON string mapping in timeline_api_handlers.cpp
- Added timeline component dependency to ui component (PRIV_REQUIRES)
- Added GetKeyingSubsystem() getter to ApplicationController for API access

**Group 3.0 - Frontend Canvas Rendering (Tasks 3.1-3.9 Complete):**
- Complete timeline.html with inline CSS, canvas element, controls, full legend (⏱ Late release, 🔒 Latch, ⚡ Squeeze, gap markers)
- Full timeline.js implementation (700+ lines): event polling (100ms), grid rendering with 1-dit WPM-aware alignment, 4-track display (DOT/DASH/OUT/LOGIC)
- LOGIC overlay rendering: Memory window shading (yellow dit/orange dah), late release symbols, latch periods, squeeze indicators, gap markers with thickness variation
- Decoded text rendering with overlap detection (auto-offset when <10px spacing)
- requestAnimationFrame 60fps render loop with off-screen canvas grid caching
- Event buffer management: circular buffer (max 10,000 events), auto-pruning (2× duration window)

**Group 4.0 - UI Controls (Tasks 4.1-4.8 Complete):**
- Duration slider (1-10s), enable/disable toggle, 6 visualization option checkboxes
- localStorage persistence (keyer_timeline_config key) with save/reset buttons
- Responsive design (mobile/desktop), purple gradient header matching project style
- Real-time WPM polling from /api/timeline/config (adaptive decoder or keying config)

**Group 5.0 - Backend Integration (Tasks 5.1-5.12 Complete):**
- Extended PaddleEngineCallbacks struct with timeline::TimelineHooks member
- Integrated TimelineEventEmitter into KeyingSubsystem (forward declaration, setter, private member)
- Implemented SetTimelineEmitter() method with hook forwarding to paddle engine callbacks
- Hook propagation: callbacks update on Initialize/ApplyConfig cycle
- PaddleEngine instrumentation: Memory window hooks (ArmMemory, OnPaddleEvent), late release hook (OnPaddleEvent), latch hooks (Tick, StartElement), squeeze detection hook (Tick)
- MorseDecoder gap markers: Element/character/word gap emission via SetTimelineLogger
- ApplicationController wiring: TimelineEventEmitter creation in SubsystemCreationPhase, injection to KeyingSubsystem/MorseDecoder in SubsystemWiringPhase
- Build fix: Added missing hal/high_precision_clock.hpp include to paddle_engine.cpp

**Files created:**
- [components/timeline/include/timeline/timeline_hooks.hpp](components/timeline/include/timeline/timeline_hooks.hpp) - Hook callback definitions
- [components/timeline/include/timeline/timeline_event_emitter.hpp](components/timeline/include/timeline/timeline_event_emitter.hpp) - Emitter class
- [components/timeline/timeline_event_emitter.cpp](components/timeline/timeline_event_emitter.cpp) - Implementation with ESP_LOG_DEBUG
- [components/ui/timeline_api_handlers.cpp](components/ui/timeline_api_handlers.cpp) - REST API handlers
- [docs/TIMELINE_HOOKS.md](docs/TIMELINE_HOOKS.md) - Hook architecture documentation
- [webui/html/timeline.html](webui/html/timeline.html) - Complete HTML5 frontend (canvas, controls, legend)
- [webui/js/timeline.js](webui/js/timeline.js) - Full visualization engine (700+ lines: rendering, polling, controls)

**Files modified:**
- [components/timeline/include/timeline/event_logger.hpp](components/timeline/include/timeline/event_logger.hpp) - EventType enum extended (5→11 types)
- [components/ui/include/ui/http_server.hpp](components/ui/include/ui/http_server.hpp) - Handler declarations added
- [components/ui/http_server.cpp](components/ui/http_server.cpp) - URI routes registered, endpoint count 17→19
- [components/ui/CMakeLists.txt](components/ui/CMakeLists.txt) - timeline dependency added
- [components/app/include/app/application_controller.hpp](components/app/include/app/application_controller.hpp) - GetKeyingSubsystem() getter
- [components/keying/include/keying/paddle_engine.hpp](components/keying/include/keying/paddle_engine.hpp) - PaddleEngineCallbacks extended with timeline_hooks member
- [components/keying_subsystem/include/keying_subsystem/keying_subsystem.hpp](components/keying_subsystem/include/keying_subsystem/keying_subsystem.hpp) - TimelineEventEmitter integration (forward declaration, setter, member)
- [components/keying_subsystem/keying_subsystem.cpp](components/keying_subsystem/keying_subsystem.cpp) - SetTimelineEmitter() implementation

**Status:**
- Frontend: ✅ Complete (Groups 3.0-4.0 - full canvas rendering, controls, localStorage)
- Backend Core: ✅ Complete (Groups 1.0-2.0 - hooks, REST API)
- Backend Integration: ✅ Complete (Group 5.0 - Tasks 5.1-5.12: PaddleEngine hooks, decoder gap markers, ApplicationController wiring)
- Build: ✅ Verified (4 pre-existing MorseTable test failures unrelated to timeline work)
- Testing: Pending unit tests and hardware validation

**Next steps:** Unit tests (Tasks 5.13-5.18), documentation updates (Tasks 5.19-5.22), hardware testing

---

### PRD Created: Real-Time Timeline Visualization

**Feature specification complete:**
- HTML5 Canvas oscilloscope-style timeline with 4 tracks (DOT/DASH/OUT/LOGIC)
- Background grid aligned to 1 dit duration (WPM-aware)
- Configurable time window (1-10 seconds slider)
- Visualization options: Memory window, late release, latch, squeeze detection, gap markers
- Decoder integration: timestamp-aligned character display
- REST API polling (100ms) with event streaming

**PRD location:** `.project-management/current-prd/prd-realtime-timeline.md`

### Bugfix: Decoder Not Initialized - Web UI Enable Failed

**Fixed critical morse decoder bug:**
- Decoder enable checkbox in Web UI had no effect (status remained "disabled")
- API endpoint `/api/decoder/enable` called methods on nullptr

**Root cause:**
- MorseDecoder instance was never created in ApplicationController initialization
- GetMorseDecoder() method existed but returned nullptr

**Fix:**
- Added decoder instantiation in SubsystemCreationPhase
- Injected decoder into KeyingSubsystem during wiring phase
- Fixed Configuration button link in decoder.html (`/config.html` → `/config`)

**Testing:**
- Build successful (1.3MB binary, 69% partition free)
- Hardware testing required to verify Web UI functionality

**Files modified:**
- [components/app/include/app/application_controller.hpp](components/app/include/app/application_controller.hpp#L204-L205)
- [components/app/application_controller.cpp](components/app/application_controller.cpp#L9-L10)
- [components/app/init_phases.cpp](components/app/init_phases.cpp#L195-L204)
- [webui/html/decoder.html](webui/html/decoder.html#L71)

---

## 2025-11-01

### Bugfix: Paddle Engine Missed Elements in Fast Transitions

**Fixed critical iambic keying bug:**
- Paddle press detected (LED ON) but no tone produced during fast dit/dah transitions
- Issue occurred when opposite paddle pressed during intra-element gap

**Root cause:**
- Paddle engine required `press_time >= gap_end_time` to consider paddle "ready"
- Ignored paddles pressed during the gap (even though actively held)

**Fix:**
- Changed condition to `press_time >= gap_start_time` (start of gap, not end)
- Paddles pressed during or after gap are now correctly recognized
- Memory window (60-99%) still enforced - early presses (e.g., 20%) rejected as designed

**Testing:**
- All 7 PaddleEngineTest unit tests passed
- Preserved behavior: `MemoryWindowRejectsEarlyPress` test still passes

**Files modified:** [components/keying/paddle_engine.cpp](components/keying/paddle_engine.cpp#L124-L145)

---

## 2025-11-01

### Web UI Fixes - Keying Parameters and Page Navigation

**Fixed Web UI issues:**
- Keying parameters not updating in real-time (event listener accumulation bug)
- Missing routes for remote.html and decoder.html pages
- No landing page with navigation to all web pages

**Changes:**
- Fixed event listener duplication in config.html by separating `addEventListeners()` function
- Added auto-refresh of config values every 3 seconds (when no unsaved changes)
- Created index.html landing page with navigation cards to all sections
- Added routes: `/config`, `/remote`, `/decoder`
- Root `/` now serves index.html with full navigation instead of config.html directly

**Commit:** cd915c0

---

## 2025-11-01

### Morse Decoder Task 7.0 COMPLETED - Configuration Parameters

**Completed missing decoder configuration parameters for Phase 2 implementation:**

**Configuration additions:**
- `decoder_buffer_size` (uint32_t, default 100) - Number of decoded characters to retain in circular buffer (range 50-500)
- `decoder_log_events` (bool, default false) - Enable verbose debug logging for decoder timing analysis

**Implementation details:**
- Added fields to `KeyingConfig` struct in [device_config.hpp](components/config/include/config/device_config.hpp)
- Added comprehensive parameter metadata in [parameters.yaml](components/config/parameters.yaml) with help text and usage examples
- NVS keys: `key_dec_bufsz` (buffer size), `key_dec_log` (logging toggle)
- All 4 decoder config parameters now complete: `decoder_enabled`, `decoder_buffer_size`, `decoder_tolerance_percent`, `decoder_log_events`

**Task status:**
- Task 7.0: ✅ COMPLETE (all configuration parameters implemented)
- Phase 2 console interface: ✅ Complete (`decoder start/stop/reset/stats/text` commands functional)
- Phase 2 Web UI: ❌ Not implemented (Task 9.0 pending)

**Documentation:**
- Updated [tasks-prd-morse-decoder.md](.project-management/current-prd/tasks-prd-morse-decoder.md) to reflect completion
- Removed PARTIAL warnings from task tracking

**Next steps:** Task 9.0 - Web UI implementation (REST API endpoints + HTML/JS/CSS frontend)

**Commit:** f3e925a
**Files modified:** 3 files changed, 101 insertions(+), 17 deletions(-)

---

## 2025-10-29

### 🚧 WIP: Paddle ISR Event Flood Bug Fix (BLOCKED BY BOOTLOOP)

**Problem:** Critical paddle hardware bug causing unpredictable keying behavior with 300+ queue overflow events per paddle press.

**Symptoms:**
- Paddle contacts shorted to ground → NO EVENTS (paddles don't work)
- Nothing connected to hardware → SPURIOUS INTERRUPTS (sees closed contacts on floating GPIO)
- Log shows: `ISR queue overflow! Dropped events: 306 (+306)` every paddle press
- Queue overflow causes unpredictable keying behavior

**Investigation findings:**
- ISR code (paddle_hal.cpp) IDENTICAL to known-good commit → not ISR itself
- UpdatePaddleActivity() called from ISR context may cause spinlock/timing issues triggering GPIO retrigger
- 300+ events per press are normal paddle bounces (1-10ms mechanical/electrical transitions)
- Previous verbose logging (commit e2c0395) masked the flood by saturating CPU
- Removing logging revealed pre-existing event flood that system couldn't handle

**Solution implemented (minimal fix):**
1. Removed UpdatePaddleActivity() from ISR context (ApplicationController::RecordPaddleEvent)
2. Moved diagnostics update to task context (KeyingSubsystem::DrainPaddleEvents at 50Hz)
3. Added diagnostics_subsystem dependency to keying_subsystem CMakeLists.txt
4. Wired diagnostics in SubsystemWiringPhase (init_phases.cpp)
5. Added forward declaration + setter + member for DiagnosticsSubsystem in KeyingSubsystem

**Changes:**
- `application_controller.cpp`: Removed diagnostics call from ISR (line 129), added TEMP GPIO debug logging every 10s
- `keying_subsystem.hpp`: Added diagnostics forward declaration, SetDiagnosticsSubsystem() setter, diagnostics_subsystem_ member
- `keying_subsystem.cpp`: Added diagnostics include, call UpdatePaddleActivity() in DrainPaddleEvents (task context), TEMP null check + debug logging
- `init_phases.cpp`: Call SetDiagnosticsSubsystem() in SubsystemWiringPhase
- `keying_subsystem/CMakeLists.txt`: Added diagnostics_subsystem to PRIV_REQUIRES

**Temporary debug code (to be removed):**
- GPIO_DEBUG logging (application_controller.cpp lines 230-241): logs GPIO config + dropped events every 10s
- Queue race condition null check (keying_subsystem.cpp lines 104-111): protects Phase 9-11 init window
- BUG DETECTED logging (keying_subsystem.cpp lines 201-206): counts pre-init dropped events

**Status:**
- ⚠️ **BLOCKED BY BOOTLOOP** - Cannot test ISR fix on hardware
- Build: ✅ Compiles successfully
- Hardware: ❌ Bootloop occurs BEFORE reaching main loop
- Bootloop affects BOTH bugfix-ISR and dev branches (not caused by ISR fix changes)

**Bootloop investigation:**
- Crash occurs AFTER "Migration completed" in ConfigStorage Phase 4 (storage.cpp:605)
- UART1 debug shows: Core dump check failed → tries to save new coredump during boot → crash → loop
- sdkconfig changes: `CONFIG_ESP_COREDUMP_CHECK_BOOT=y`, `CONFIG_FREERTOS_ISR_STACKSIZE=2096`, `CONFIG_TINYUSB_CDC_TX_BUFSIZE=4096`
- Disabling `CONFIG_ESP_COREDUMP_CHECK_BOOT` had NO EFFECT
- Suspected causes: `CONFIG_ESP_COREDUMP_STACK_SIZE=0`, device_config struct assignment crash, NVS corruption

**Next steps:**
1. Restore sdkconfig to known-good commit (cef8448a)
2. Incremental testing: add ONE sdkconfig change at a time to identify culprit
3. If sdkconfig not the cause: investigate struct assignment crash with JTAG
4. After bootloop resolved: test ISR fix on hardware and verify 300+ event flood eliminated

**Commit:** 771a70f
**Files:** 6 changed, 58 insertions(+), 6 deletions(-)
**Branch:** bugfix-ISR

---

## 2025-10-28

### Serial Console Task 4.0 & 5.0 COMPLETED - Debug Commands & Advanced User Experience

**Implemented professional command-line interface on USB-CDC1 (COM7) with runtime debugging and advanced line editing:**

**Task 4.0 - Debug & System Commands:**
- Debug commands: `debug level <level>` (none/error/warn/info/debug/verbose) for global log control
- Tag-specific logging: `debug show/hide <tag>` with common tags (keying, audio, wifi, config, storage, app, paddle, sidetone, timeline, http_server)
- Bulk control: `debug show/hide all` for all common subsystem tags
- Status display: `show debug` special case for current debug settings
- System commands: `reboot confirm` and `factory-reset confirm` with mandatory confirmation safety
- Safety features: Destructive commands require explicit "confirm" argument, 500ms delay before restart for message transmission

**Task 5.0 - Advanced User Experience:**
- Command history: 8-entry circular buffer (1KB) with up/down arrow navigation (ESC [ A/B sequences)
- Cursor movement: Left/right arrow keys (ESC [ D/C) with position tracking throughout line
- Line editing: Cursor-aware backspace, character insertion at cursor position with auto-redraw
- TAB completion: Command name completion with unique match auto-complete, ambiguous match listing
- ESC sequence parser: 3-state machine (NORMAL → ESC_RECEIVED → CSI_RECEIVED) for ANSI escape sequences
- CRLF handling: Robust `\r\n` line endings for Windows/Linux terminal compatibility

**Implementation details:**
- State tracking: `histNav_` for history navigation, `cursorPos_` for cursor position, `escState_` for ESC sequences
- Helper methods: `redrawLine()` (4-step process), `moveCursorLeft/Right()` for terminal control
- Thread safety: FreeRTOS task with 8KB stack, mutex-protected parameter access
- Code quality: Detailed inline comments explaining state machines, circular buffer logic, multi-byte sequence handling

**Binary size:** 0x131940 bytes (1.19 MB, 70% free, +1.5KB from previous)

**Commits:** 2 commits (1d7ae4d, c3afbc2)

**Files modified:**
- [components/ui/InputHandler.hpp](components/ui/InputHandler.hpp) - Added ESC state machine, cursor tracking, history navigation
- [components/ui/InputHandler.cpp](components/ui/InputHandler.cpp) - Implemented arrow keys, TAB completion, advanced editing (+263 lines)
- [components/ui/console_parameter_bridge.cpp](components/ui/console_parameter_bridge.cpp) - Debug/system command handlers (+233 lines)
- [components/ui/console_parameter_bridge.hpp](components/ui/include/ui/console_parameter_bridge.hpp) - Command declarations (+49 lines)
- [components/ui/serial_console.hpp](components/ui/include/ui/serial_console.hpp) - GetCommands() for TAB completion
- [components/ui/serial_console.cpp](components/ui/serial_console.cpp) - GetCommands() implementation

**Total code change:** +592 lines, -40 lines (net +552 lines with extensive comments)

**Testing:** Build verification completed, manual testing requires hardware (USB-CDC interface)

**Commands available:**
- Parameter: `show [*|param|prefix]`, `set <param> <value>`, `save`
- Debug: `debug level|show|hide <args>`, `show debug`
- System: `reboot confirm`, `factory-reset confirm`
- Built-in: `help [command]`

---

## 2025-10-27

### Feature 3 COMPLETED - Macro-Driven Parameter Table Refactoring

**Implemented single source of truth for all 40 device parameters using compile-time metadata:**
- Created [parameter_table.hpp](components/config/include/config/parameter_table.hpp) with PARAMETER_TABLE macro defining all parameters (general, hardware, audio, keying, WiFi)
- Implemented table-driven NVS persistence: LoadParameter() and SaveParameter() replacing 700+ lines of manual nvs_get/nvs_set calls
- Refactored parameter_registry.cpp to auto-generate registrations from PARAMETER_TABLE (812→248 lines, -69%)
- Implemented all-or-nothing configuration validation with automatic backup fallback
- Added template helpers to eliminate code duplication in NVS load/save operations

**Code reduction achieved:**
- LoadOrDefault: 213→47 lines (-78%, complexity reduced from ~50 branches to ~10)
- parameter_registry.cpp: 812→248 lines (-69%, 564 lines eliminated)
- storage.cpp: 928→582 lines (-35%, 346 lines eliminated)
- Total: ~1200 lines of code eliminated

**Maintainability improvement:**
- Add new parameter: 5 files × 30 lines → **1 line** in PARAMETER_TABLE macro
- Type-safe compile-time validation using offsetof() and decltype()
- Single source of truth eliminates consistency issues between storage and registry
- Automatic backup before save with LoadPreviousOrDefaults() fallback chain

**Binary size:** 0xf7d40 bytes (76% free, unchanged from refactoring)

**Commits:** 8 commits (cef8448, 8995d78, 75238dd, f3fa8df, b0847f5, 85e55e6, plus documentation commits)

**Files created:**
- [components/config/include/config/parameter_table.hpp](components/config/include/config/parameter_table.hpp) - 396 lines
- [components/config/parameter_table.cpp](components/config/parameter_table.cpp) - 25 lines

**Files refactored:**
- [components/config/storage.cpp](components/config/storage.cpp) - Major refactoring
- [components/config/parameter_registry.cpp](components/config/parameter_registry.cpp) - Macro-driven generation
- [components/config/include/config/device_config.hpp](components/config/include/config/device_config.hpp) - Updated method declarations

**Testing:** Build verification completed, hardware testing deferred to user

### Feature 4 COMPLETED - Parameter Metadata Unification with YAML Code Generation

**Unified parameter systems (PARAMETER_TABLE + ParameterRegistry) with YAML-driven code generation:**
- Created [parameters.yaml](components/config/parameters.yaml) as single source of truth for all 40 parameters with comprehensive help text
- Implemented Python code generator ([generate_parameters.py](components/config/scripts/generate_parameters.py)) producing both HPP and CPP files
- Integrated CMake custom command for automatic code regeneration on YAML changes
- Generated files excluded from Git (.gitignore strategy for build artifacts)
- Eliminated 80+ manual lambda registrations with type-safe auto-generated code

**Code reduction achieved:**
- parameter_registry.cpp: 249→165 lines (-84 lines, -34%) - removed all macro-based registrations
- Total manual code eliminated: ~400 lines of parameter definitions
- Generated code: parameter_table.hpp (218 lines), parameter_registry_generated.cpp (335 lines)
- Help text system: 120-200 usage examples across 40 parameters

**Workflow improvement:**
- Add parameter: **1 macro line** → **10-15 YAML lines** with rich documentation
- Single edit triggers automatic regeneration of both storage and registry code
- JSON Schema validation catches errors at build time with clear error messages
- Help text (short/long/examples) embedded in YAML for comprehensive documentation

**Generated file structure:**
- parameter_table.hpp: Complete type system (NvsType enum, ParameterDescriptor struct, validators) + PARAMETER_TABLE macro
- parameter_registry_generated.cpp: RegisterAllParameters() with 40 type-safe getter/setter lambdas
- Auto-generated headers: "DO NOT EDIT - Changes will be overwritten on build!" with timestamp

**Binary size:** 0xf7d50 bytes (76% free, unchanged - code generation maintains identical binary output)

**Commits:** 5 commits (cacdc41, 8fbf419, 672a672, 6ff5d75, d234be3)

**Files created:**
- [components/config/parameters.yaml](components/config/parameters.yaml) - 1445 lines (YAML with help text)
- [components/config/parameters_schema.json](components/config/parameters_schema.json) - JSON Schema validation
- [components/config/scripts/generate_parameters.py](components/config/scripts/generate_parameters.py) - 415 lines Python generator
- components/config/include/config/parameter_table.hpp - Generated (218 lines)
- components/config/parameter_registry_generated.cpp - Generated (335 lines)

**Files refactored:**
- [components/config/CMakeLists.txt](components/config/CMakeLists.txt) - Added Python code generation integration
- [components/config/parameter_registry.cpp](components/config/parameter_registry.cpp) - Removed macro-based registrations
- [.gitignore](.gitignore) - Excluded generated files

**Testing:**
- Build verification: fullclean + build from scratch ✓
- Functional: 40 parameters registered, type-safe lambdas ✓
- NVS storage: All keys match existing format (no breaking changes) ✓
- Code quality: Zero compilation warnings with -Werror=all ✓
- Error handling: Clear validation messages for malformed YAML ✓

---

## 2025-10-26

### Task 5.4.3 - Web UI Complete Configuration Page

**Implemented comprehensive web configuration interface:**
- Single-page application embedded in HTTP server (~13KB HTML+CSS+JS inline)
- Auto-generated forms from `/api/config/schema` with 5 widget types: number_input, slider, dropdown, checkbox, text_input
- Modern responsive design: gradient header, tabbed navigation (general/audio/keying/wifi/hardware), real-time status bar
- Save workflow: atomic parameter updates via POST `/api/config/{subsystem}/{param}` + POST `/api/config/save` for NVS persistence
- Alert system with success/error feedback and auto-dismiss
- Real-time status polling (3s interval): WiFi mode, IP, keying preset, WPM
- Conditional visibility support (e.g., keying window parameters only shown when preset==MANUAL)
- Mobile-friendly with 768px responsive breakpoint

**Removed obsolete WiFi-specific code:**
- Deleted `HandleGetWiFiConfig()` and `HandlePostWiFiConfig()` handlers (WiFi now managed via general parameter system)
- Removed `/api/config/wifi` GET/POST endpoints (2 endpoints)
- Removed `ParseFormField()` helper method
- Cleaned up endpoint registration (8→6 endpoints)

**Binary size:** 1,008KB (4% free, +608KB from 400KB baseline due to WiFi/TCP/HTTP stack + Web UI)

**Files modified:**
- [components/ui/http_server.cpp](components/ui/http_server.cpp) - Embedded kConfigPage, removed WiFi handlers
- [components/ui/include/ui/http_server.hpp](components/ui/include/ui/http_server.hpp) - Updated handler declarations

**Testing:** Build successful, functional verification deferred to hardware testing by user

### Bug Fix - HTTP URI Wildcard Limitation (Critical)

**Fixed 404 errors for all parameter POST endpoints discovered during hardware testing:**

**Problem:** ESP-IDF HTTP server wildcard `*` matches only **single path segment**, not multiple segments:
- `/api/config/*` matches `/api/config/save` ✓
- `/api/config/*` does NOT match `/api/config/audio/freq` ✗ (two segments)

**Solution:** Redesigned API to use single endpoint without wildcards:
- **OLD:** POST `/api/config/{subsystem}/{param}` with body `{"value": "..."}`
- **NEW:** POST `/api/parameter` with body `{"param": "subsystem.param", "value": "..."}`

**Changes:**
- Backend: New `/api/parameter` endpoint parses both `param` and `value` from JSON body
- Frontend: JavaScript updated to send parameter name as `"audio.freq"` instead of split path
- Removed wildcard dependency completely - cleaner, more maintainable API

**Impact:** Without this fix, Web UI was completely non-functional. All 38 parameter updates returned 404.

**Testing:** Build successful (1,008KB binary unchanged), ready for hardware verification

### Flash Expansion: 2MB → 16MB with OTA Dual-Partition Support

**MAJOR UPGRADE:** Expanded flash utilization from 2MB to full 16MB physical flash capacity with production-ready OTA partition scheme.

**Previous Configuration (2MB):**
```
nvs      : 24KB
phy_init : 4KB
factory  : 1MB  (app partition, 4% free = 39KB)
TOTAL    : 1.02MB (85% of 2MB flash WASTED)
```

**New Configuration (16MB OTA):**
```
nvs      : 96KB    (4x expansion for extensive config storage)
otadata  : 8KB     (OTA state management)
phy_init : 4KB     (WiFi calibration data)
ota_0    : 4MB     (primary app partition, 76% free = 3,121KB!)
ota_1    : 4MB     (backup app partition for OTA updates)
spiffs   : 7.98MB  (file system for logs/waveforms/user data)
TOTAL    : ~16MB   (99% flash utilization)
```

**Improvements:**
- ✅ **App partition:** 1MB → 4MB (**4x expansion**)
- ✅ **Free space:** 39KB (4%) → 3,121KB (76%) (**80x more margin**)
- ✅ **OTA updates:** Remote firmware updates via WiFi (production-ready)
- ✅ **Rollback safety:** Automatic fallback if OTA fails
- ✅ **SPIFFS support:** 7.98MB storage for future features (logs, recordings, profiles)
- ✅ **NVS expansion:** 24KB → 96KB (4x for extensive configuration)

**Breaking Change:**
⚠️ **Requires complete re-flash** - partition table incompatible with previous 2MB layout
⚠️ **NVS data will be lost** - backup configuration before flashing if needed

**Files:**
- [partitions.csv](partitions.csv) - NEW custom partition table
- [sdkconfig](sdkconfig) - Updated flash size (2MB→16MB) and partition config (SINGLE_APP→CUSTOM)

**Flash Command (updated):**
```bash
idf.py -p /dev/ttyACM0 flash
# Now flashes: bootloader + partition table + ota_data + firmware
```

**Binary size:** 1,009KB (unchanged, now uses only 25% of 4MB ota_0 partition)

### Initialization Pipeline Refactoring - ApplicationController God Method Eliminated

**Refactored 199-line monolithic ApplicationController::Initialize() into modular 16-phase pipeline using Builder + Strategy pattern:**
- Created InitPhase interface with Execute(), GetName(), IsCritical() virtual methods
- Implemented InitializationPipeline orchestrator with phase registration and error handling delegation
- Split initialization logic into 16 concrete phase classes (15-40 LOC each) with single responsibility:
  - **Critical infrastructure** (3): UartDebugPhase, NvsFlashPhase, HighPrecisionClockPhase
  - **Configuration** (2): ConfigStoragePhase, ParameterRegistryPhase
  - **Diagnostics + USB** (2): DiagnosticsSubsystemPhase, UsbEarlyInitPhase
  - **Subsystem creation** (1): SubsystemCreationPhase
  - **Subsystem initialization** (4): PaddleHalPhase, TxHalPhase, KeyingSubsystemPhase, AudioSubsystemPhase
  - **Dependency wiring** (1): SubsystemWiringPhase
  - **Network services** (2): WiFiSubsystemPhase, HttpServerPhase
  - **Watchdog** (1): WatchdogPhase
- Explicit hardware dependency ordering enforced by phase registration sequence (UART→NVS→Clock→Config→Subsystems→Network)
- LED boot phase signaling preserved at identical checkpoints (Magenta→Cyan→Orange→Yellow→Green)

**Code reduction achieved:**
- ApplicationController::Initialize(): 199→48 lines (-76% total, -88% code-only excluding comments)
- Complexity reduction: 16 inline blocks with implicit dependencies → 16 explicit phases with constructor injection
- Average phase size: 22 LOC (range 15-40), each with single clear responsibility

**Architecture benefits:**
- **Testability:** Each phase can be unit-tested in isolation with mock dependencies (InitializationPipeline supports mock phases)
- **Single Responsibility Principle:** Each phase has one initialization task
- **Explicit Dependencies:** Constructor injection makes all dependencies visible
- **Clear Error Handling:** Critical vs non-critical failures explicitly declared via IsCritical()
- **Extensibility:** Adding new phases requires only implementing InitPhase interface, no modification of existing code
- **Readability:** Pipeline orchestration replaces complex procedural initialization

**Error handling strategy:**
- Critical phases: Failure calls ApplicationController::FatalInitError() → LED blinking + serial log + halt
- Non-critical phases: Failure logged with ESP_LOGE() priority, pipeline continues to next phase

**Binary size:** 0xf9130 bytes (76% free, unchanged from baseline)

**Commits:** 2 commits covering infrastructure + implementation (Phase 1-2, tests deferred)

**Files created:**
- [components/app/include/app/init_phase.hpp](components/app/include/app/init_phase.hpp) - 190 lines (InitPhase interface + InitializationPipeline class)
- [components/app/init_pipeline.cpp](components/app/init_pipeline.cpp) - 65 lines (Pipeline execution logic)
- [components/app/include/app/init_phases.hpp](components/app/include/app/init_phases.hpp) - 430 lines (16 phase class declarations)
- [components/app/init_phases.cpp](components/app/init_phases.cpp) - 355 lines (16 phase implementations)
- [tests_host/init_pipeline_test.cpp](tests_host/init_pipeline_test.cpp) - 115 lines (Google Test unit tests, compilation deferred)

**Files refactored:**
- [components/app/application_controller.cpp](components/app/application_controller.cpp) - Initialize() method refactored (199→48 LOC)
- [components/app/include/app/application_controller.hpp](components/app/include/app/application_controller.hpp) - Added friend declarations, moved FatalInitError() to public
- [components/app/CMakeLists.txt](components/app/CMakeLists.txt) - Added init_pipeline.cpp and init_phases.cpp to build
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) - Added comprehensive pipeline architecture documentation with ASCII diagram

**Testing:**
- Build verification: Firmware compiles successfully, binary size unchanged ✓
- Unit tests: Created MockPhase-based tests for pipeline orchestration, compilation deferred due to concurrent development
- Integration testing: Deferred to user (boot sequence, LED signals, console commands)

**Documentation:**
- Added "Application Initialization Pipeline" section to [DEVELOPMENT.md](DEVELOPMENT.md) with:
  - Architecture overview and InitPhase interface specification
  - 16-phase boot sequence with ASCII diagram showing execution order
  - Critical vs non-critical error handling explanation
  - LED boot phase signaling table (0: Magenta → 4: Green)
  - Complete guide for adding new initialization phases
  - Benefits summary and code references

2025-10-27T09:32Z - Implemented GUI parameter hot-reload: parameter changes from Web UI now apply immediately without device reset (keying speed/preset, audio frequency/volume/fade)
2025-10-27T09:40Z - Added hardware parameter reset warnings: Web UI now displays warning banner when GPIO/I2C/I2S parameters are changed, informing user that device reset is required
2025-10-27T09:51Z - Documented 5 Priority 1 critical files with comprehensive architecture rationale and usage examples (diagnostics_subsystem.cpp, device_config.hpp, sidetone_service.hpp, startup.cpp/.hpp)
2025-10-27T16:19:01Z - Swapped manual JSON assembly for cJSON across registry/http server to improve correctness and type fidelity
2025-10-27T16:55:52Z - Externalized Web UI assets: added build pipeline for gzip-embedded pages, refactored HttpServer to serve blobs, introduced timeline stub route
2025-11-01T20:10Z - Fixed 8 critical bugs in single session: (1) Keying engine ignoring fast paddle transitions (gap timing logic), (2) Decoder not initialized in ApplicationController, (3) Decoder config not applied from device_config, (4) Console commands missing decoder registration, (5) Console cursor stuck at start after history/tab (2-part fix: redrawLine condition + moveCursorRight logic), (6) HTTP URI handler limit 22→32 (decoder.js 404), (7) Decoder timeout dynamic WPM-based (7×dit word gap). All commits re-authored: Simo IU3QEZ. Binary: 1.3MB (69% free). Testing: pending hardware verification.
2025-11-01T21:15Z - Fixed decoder Web UI frozen display bug: UI stopped updating when buffer reached 100 characters because JavaScript compared text length instead of content. Changed lastTextLength to lastText with full string comparison. Impact: UI now updates continuously even with full circular buffer.
2025-11-03T15:30Z - Completed paddle_engine rewrite (tasks 1.0-5.0, 48/50 tasks): Replaced 3-state FSM with clean 4-state model (IDLE/SEND_DIT/SEND_DAH/GAP) based on docs/state_machine.py prototype. New enums: IambicMode (A/B), MemoryMode (None/DotOnly/DahOnly/Both), SqueezeMode (Snapshot/Live). Removed LateReleaseBehavior::kForget and OnLateReleaseActive hook. Added configurable memory windows (mem_block_start_pct/end_pct), Mode B squeeze bonus logic, timeline hooks (OnMemoryWindowChanged, OnSqueezeDetected). Implemented callbacks (on_element_started/finished, on_key_state_changed), config validation in Initialize(), complete Reset()/DumpState(). Build: successful (no errors). Tests: 14/14 PaddleEngineTest passed. Pending: manual hardware testing (5.9), final documentation (5.10).
2025-11-07T15:45Z - Fixed remote CW client CONNECT handshake: Changed permissions field from big-endian (lwip_htonl) to little-endian native per CWNet protocol spec. Updated protocol doc payload size from 88 to 92 bytes (44+44+4).
