# WiFi Subsystem Exploration Report
## Complete Analysis of Current Implementation

---

## 1. CURRENT WiFi CAPABILITIES

### WiFi Modes Supported
The firmware supports **dual-mode WiFi operation**:

1. **Station (STA) Mode** - Primary
   - Connects to existing WiFi network (SSID + password)
   - Uses DHCP for IP address
   - Monitors signal strength (RSSI in dBm)
   - Auto-reconnects on disconnect
   - Configurable connection timeout (5-300 seconds, default 30s)

2. **Access Point (AP) Mode** - Fallback
   - Creates own WiFi hotspot
   - Fixed IP: 192.168.4.1 (no DNS server configured)
   - Supports up to 4 connected clients
   - Optional WPA2 security (or open network)
   - Automatic fallback when STA fails

### State Machine
```
IDLE → STA Connecting → STA Connected
       ↓ (timeout/fail)
       AP Active ← (fallback enabled)
```

### Key Characteristics
- **ESP32-S3 Limitation**: Cannot run STA + AP simultaneously
- **Max Retries**: 3 attempts with 5s delay between retries
- **AP Fallback**: Configurable (enable/disable via `wifi.fallback` parameter)
- **Event-Driven**: Uses ESP-IDF event loop for connection/disconnection handlers

---

## 2. CONFIGURATION STRUCTURE

### WiFiConfig Struct (device_config.hpp:203-230)

```cpp
struct WiFiConfig {
  char sta_ssid[32] = "";           // Station SSID (empty = disabled)
  char sta_password[64] = "";       // Station password (WPA2-PSK)
  char ap_ssid[32] = "Keyer-AP";    // Access Point SSID
  char ap_password[64] = "";        // AP password (empty = open)
  bool enable_ap_fallback = true;   // Enable AP mode on STA failure
  uint16_t sta_timeout_sec = 30;    // Connection timeout before fallback
};
```

### Optional WiFi Secrets (Development)
Support for compile-time defaults from `config/wifi_secrets.h`:
- `WIFI_DEV_SSID` - Development STA SSID
- `WIFI_DEV_PASSWORD` - Development STA password
- `WIFI_AP_SSID` - AP SSID override
- `WIFI_AP_PASSWORD` - AP password override

---

## 3. NETWORK CONFIGURATION PARAMETERS

Parameters are defined in `components/config/parameters.yaml` and registered dynamically:

### WiFi Parameters (Subsystem: wifi)

| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| `wifi.sta_ssid` | STRING | 0-31 chars | "" | Station SSID (empty = STA disabled) |
| `wifi.sta_password` | STRING | 0-63 chars | "" | Station password (WPA2) |
| `wifi.ap_ssid` | STRING | 4-31 chars | "Keyer-AP" | AP broadcast SSID |
| `wifi.ap_password` | STRING | 0-63 chars | "" | AP password (empty = open) |
| `wifi.fallback` | BOOL | 0/1 | true | Enable AP fallback |
| `wifi.sta_timeout` | UINT16 | 5-300 sec | 30 | STA connection timeout |

All parameters are:
- ✅ Stored in NVS (encrypted if enabled)
- ✅ Accessible via console commands (`wifi <param> <value>`)
- ✅ Accessible via Web UI (Config page, dynamic forms)
- ✅ Runtime-modifiable without device reset
- ✅ Self-documented with help text

**AP SSID Minimum:** 4 characters required (checked in schema)

---

## 4. EXISTING WiFi CONNECTION LOGIC

### WiFiSubsystem Class
**File:** `components/wifi_subsystem/wifi_subsystem.cpp`
**Size:** 418 lines

#### Core Methods
1. **Initialize()** - Set up WiFi stack
   - Initializes ESP-IDF netif, event loop, WiFi driver
   - Registers WIFI_EVENT and IP_EVENT handlers
   - Starts STA (if SSID configured) or AP (if fallback enabled)

2. **Tick()** - Main loop monitoring (call every ~100ms)
   - Monitors STA connection timeout
   - Triggers AP fallback after 3 failed retries
   - Updates status cache

3. **StartStaMode()** - Connect to existing network
   - Sets WiFi mode to WIFI_MODE_STA
   - Configures SSID/password (WPA2-PSK minimum)
   - Starts connection attempt

4. **StartApMode()** - Create hotspot
   - Sets WiFi mode to WIFI_MODE_AP
   - Configures SSID (always broadcast)
   - Optional WPA2 password (open if empty)
   - Max 4 clients, 100ms beacon interval

5. **GetStatus()** - Thread-safe status query
   - Returns current mode (STA Connecting/Connected, AP Active)
   - IP address (STA via DHCP, AP = 192.168.4.1)
   - RSSI (STA only), client count (AP only)
   - Ready flag for HTTP server startup

6. **Reconnect()** - Runtime reconfiguration
   - Updates WiFi credentials via SetConfig()
   - Stops current mode, restarts with new config

#### Event Handlers
- `WIFI_EVENT_STA_START` - Trigger connection
- `WIFI_EVENT_STA_CONNECTED` - AP joined
- `WIFI_EVENT_STA_DISCONNECTED` - Disconnect or timeout
- `IP_EVENT_STA_GOT_IP` - DHCP assigned → ready for HTTP server
- `WIFI_EVENT_AP_START` - AP started → immediately ready
- `WIFI_EVENT_AP_STACONNECTED/DISCONNECTED` - Client join/leave logging

#### Diagnostics Integration
- Receives optional pointer to DiagnosticsSubsystem
- Calls `SignalWifiError()` on init failure (red flash)
- Calls `SignalWifiConnected()` on IP acquired (animation)
- Calls `SignalCheckpoint()` for phase signaling

---

## 5. WEB SERVER INFRASTRUCTURE

### HTTP Server (http_server.cpp)
**File:** `components/ui/http_server.cpp`
**Size:** 1415 lines
**Port:** 80 (hardcoded)
**Status:** ✅ Running when WiFi ready (STA connected OR AP active)

#### Current URL Handlers

**Configuration Pages:**
- `GET /` → Serve `index.html` (Svelte SPA)
- `GET /config` → Serve `index.html` (same SPA, different route)
- `GET /timeline`, `/remote`, `/decoder`, `/system`, `/firmware` → Serve `index.html`

**REST API Endpoints:**
- `GET /api/status` → WiFi/system status (mode, IP, RSSI, keying)
- `GET /api/config/schema` → Parameter schema with widget hints (JSON)
- `GET /api/config` → Current configuration (all parameters)
- `POST /api/parameter` → Update single parameter
- `POST /api/config/save` → Persist to NVS
- `GET /api/timeline/events` → Event log for visualization
- `GET /api/remote/status` → Remote keying status
- `GET /api/decoder/status` → Morse decoder status

**Static Assets:**
- Embedded HTML/JS/CSS (gzip-compressed)
- Generated from `webui/` directory via `embed_assets.py`
- Served with MIME types and CORS headers

### Web UI Framework
**Framework:** Svelte (not SvelteKit)
**Location:** `/home/user/keyer_qrs2hst/webui/`
**Build Tool:** Vite
**Pages Implemented:**
- `Home.svelte` - Status dashboard
- `Config.svelte` - Parameter editor (dynamic)
- `Keyer.svelte` - Text keyer (CW transmission)
- `FirmwareUpdate.svelte` - OTA/UF2 update
- `Decoder.svelte` - Morse decoder view
- `Remote.svelte` - CWNet remote keying
- `Timeline.svelte` - Event visualization
- `System.svelte` - System stats

**Asset Pipeline:**
1. Edit files in `webui/src/` and `webui/css/`
2. Build runs `scripts/webui/embed_assets.py` (CMake dependency)
3. Generates `components/ui/generated/web_assets_data.inc` (gitignored)
4. Compiled into firmware binary
5. Runtime: `ui::assets::Find(path)` retrieves from memory

---

## 6. CAPTIVE PORTAL & DNS ANALYSIS

### Current State: ❌ NOT IMPLEMENTED

**No existing code for:**
- ❌ DNS server (mDNS or simple DNS)
- ❌ Captive portal detection responses
- ❌ Forced HTTP redirect (for browsers)
- ❌ DHCP server configuration

### Access Methods Currently Available

#### In Station Mode (STA Connected)
✅ Access via DHCP-assigned IP address
- Example: `http://192.168.x.x`
- Requires knowledge of device IP
- User must manually navigate (no auto-redirect)

#### In Access Point Mode (AP)
✅ Access via fixed IP: `http://192.168.4.1`
- User manually enters IP in browser
- No automatic browser redirect
- Works on any OS/device

### Missing Pieces for Captive Portal

1. **DNS Server**
   - No mDNS implementation
   - No wildcard DNS responses
   - Browser detection fails (OS pings DNS for captive portal checks)

2. **DHCP Server** (AP mode)
   - Hardcoded AP IP only
   - No DHCP configured
   - Clients must use static IP or manual config

3. **HTTP Redirect**
   - No "Location" header redirect for non-captive requests
   - No path-based routing (e.g., `/apple.json`, `/msftncsi.txt`)
   - Browsers won't auto-redirect to setup page

4. **Web UI Awareness**
   - No WiFi setup page dedicated to initial configuration
   - Config page mixed with runtime controls
   - No "enter WiFi credentials" workflow

---

## 7. KEY ARCHITECTURAL INSIGHTS

### Memory Layout
```
WiFiSubsystem:
  - config_: WiFiConfig struct (32+64+32+64 = 192 bytes)
  - mode_: atomic<WiFiMode> (1 byte)
  - ready_: atomic<bool> (1 byte)
  - netif_sta_, netif_ap_: esp_netif_t pointers
  - Event handlers: ESP-IDF internal
  Total heap: ~20 KB (WiFi stack + event loop)
```

### Thread Safety
| Method | Thread-Safe | Notes |
|--------|-------------|-------|
| `Initialize()` | ❌ No | Call from main task only |
| `Deinitialize()` | ❌ No | Call from main task only |
| `Tick()` | ❌ No | Call from main loop only |
| `GetStatus()` | ✅ Yes | Atomic state access |
| `SetConfig()` | ❌ No | Requires external sync |
| `Reconnect()` | ❌ No | Call from main task |

### WiFi Storage
- Configuration persisted via NVS (non-volatile storage)
- Stored as blobs per subsystem
- Auto-migration on config version changes
- Backup/restore support for factory reset

### Parameter Visibility
All WiFi parameters are:
- ✅ Auto-generated from `parameters.yaml`
- ✅ Accessible via console commands
- ✅ Available in Web UI configuration
- ✅ Type-validated at registration time
- ✅ Min/max constraints enforced

---

## 8. AVAILABLE LEVERAGE POINTS FOR CAPTIVE PORTAL

### Existing Infrastructure to Extend

1. **HTTP Server**
   - ✅ Already listening on port 80 (AP mode)
   - ✅ Can add DNS redirect handler
   - ✅ Can add new URI handlers easily
   - ✅ Handler context already supports config/storage/wifi pointers

2. **WiFi Status**
   - ✅ `GetStatus()` returns WiFi mode/IP/readiness
   - ✅ Can query from HTTP handlers
   - ✅ Can serve different content based on connection state

3. **Configuration System**
   - ✅ Parameter metadata available via `/api/config/schema`
   - ✅ Can build dynamic setup forms from schema
   - ✅ Can save WiFi credentials via existing `/api/parameter` POST

4. **Web UI Assets**
   - ✅ Svelte SPA infrastructure ready
   - ✅ Can create new page component (e.g., `Setup.svelte`)
   - ✅ Asset pipeline auto-embeds changes

5. **Diagnostics**
   - ✅ NeoPixel LED control available
   - ✅ Can signal "setup mode" with LED patterns
   - ✅ Can signal "WiFi connected" with animations

6. **Application Controller**
   - ✅ Has init phases system
   - ✅ Can add WiFiSetupServer phase
   - ✅ Can dynamically enable/disable features

### Missing Components to Implement

1. **DNS Server**
   - Need to handle DNS queries on UDP/53
   - Respond with device IP for any domain
   - Only active in AP mode

2. **Captive Portal Detector**
   - Add handlers for standard probe paths:
     - `/apple.json` - iOS
     - `/msftncsi.txt` - Windows
     - `/success.txt` - Android legacy
     - `/generate_204` - Android

3. **Setup Page**
   - Dedicated Svelte component for initial WiFi setup
   - Simpler than Config page (only WiFi section)
   - Status indicators (network scan results, etc.)

4. **DHCP Server (AP mode)**
   - Currently not needed (fixed AP IP works)
   - Would improve user experience
   - Requires ESP-IDF dhcp_server component

---

## 9. INITIALIZATION PIPELINE

WiFi subsystem integrated at **Phase 14** (WiFiSubsystemPhase):

```
Phase 14: WiFiSubsystemPhase (init_phases.cpp)
  ├─ Load WiFiConfig from NVS
  ├─ Call WiFiSubsystem::Initialize()
  ├─ Start STA or AP based on config
  └─ Set status LED to yellow (connecting/waiting)

Phase 16: HttpServerPhase
  ├─ Wait for WiFi ready (status.ready == true)
  │  ├─ STA mode: ready after IP_EVENT_STA_GOT_IP
  │  └─ AP mode: ready immediately after startup
  └─ Start HTTP server on port 80
```

---

## 10. SUMMARY TABLE

| Aspect | Status | Notes |
|--------|--------|-------|
| **STA Mode** | ✅ Fully implemented | DHCP, auto-reconnect, configurable timeout |
| **AP Mode** | ✅ Fully implemented | Fixed IP 192.168.4.1, open or WPA2 security |
| **WiFi Parameters** | ✅ Complete | YAML-driven, auto-generated registry |
| **HTTP Server** | ✅ Active | Port 80, Svelte SPA, REST API |
| **Web Configuration** | ✅ Full UI | Dynamic forms from parameter schema |
| **DNS Server** | ❌ Missing | No mDNS or wildcard DNS |
| **DHCP Server** | ❌ Not needed | Fixed AP IP works, but DHCP would be nice |
| **Captive Portal Detect** | ❌ Missing | No `/apple.json`, `/msftncsi.txt` handlers |
| **Auto-Redirect** | ❌ Missing | No HTTP Location header redirects |
| **Setup Wizard** | ❌ Missing | No dedicated initial WiFi config page |
| **LED Signals** | ✅ Partial | Error/connected signals, no setup mode signal |

---

## 11. RECOMMENDED IMPLEMENTATION STRATEGY

### Phase 1: DNS + Captive Portal Detection (Quick Win)
1. Add simple DNS server (UDP/53 in AP mode)
2. Wildcard all domain queries to device IP (192.168.4.1)
3. Add HTTP handlers for standard probe paths
4. Minimal code (~200 lines), immediate browser redirect

### Phase 2: Setup Page (Good UX)
1. Create `Setup.svelte` page component
2. Display WiFi network scan results
3. Form to enter SSID + password
4. Save via existing `/api/parameter` endpoint

### Phase 3: DHCP Server (Polish)
1. Enable ESP-IDF dhcp_server component
2. Configure in AP mode initialization
3. Assign IPs from 192.168.4.2-192.168.4.254
4. Improves compatibility with all OS/devices

### Phase 4: Automatic Mode Selection
1. If STA SSID empty → start AP immediately
2. If STA SSID set → try STA, fallback to AP
3. Add "Setup Mode" button to revert to AP
4. LED pattern to indicate current mode

