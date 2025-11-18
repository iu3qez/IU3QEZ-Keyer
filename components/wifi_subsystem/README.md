# WiFi Subsystem

## Overview

WiFi subsystem component providing STA (Station) and AP (Access Point) connectivity with automatic fallback. Designed for robust network access in embedded keyer applications.

## Features

- **Station Mode (STA)**: Connect to existing WiFi network
  - Auto-retry with configurable timeout
  - Reconnection on disconnect
  - RSSI monitoring

- **Access Point Mode (AP)**: Create WiFi hotspot
  - Automatic fallback when STA fails
  - Open or WPA2-PSK security
  - Client connection monitoring

- **Configuration Persistence**: WiFi credentials stored in NVS
- **Event-Driven**: ESP-IDF event loop integration
- **Status Monitoring**: Thread-safe status queries

## Architecture

```
┌─────────────────────┐
│  ApplicationController  │
└──────────┬──────────┘
           │
           ├─ Initialize(WiFiConfig)
           ├─ Tick(now_ms)
           └─ GetStatus()
           │
┌──────────▼──────────┐
│  WiFiSubsystem      │
├─────────────────────┤
│ - StartStaMode()    │
│ - StartApMode()     │
│ - EventHandler()    │
│ - FallbackToAp()    │
└──────────┬──────────┘
           │
           │ esp_wifi_*
           ▼
┌─────────────────────┐
│  ESP-IDF WiFi Stack │
└─────────────────────┘
```

## Usage

### 1. Initialization

```cpp
#include "wifi_subsystem/wifi_subsystem.hpp"
#include "config/device_config.hpp"

wifi_subsystem::WiFiSubsystem wifi_subsystem;
config::WiFiConfig wifi_config;

// Load WiFi config from NVS
wifi_config.sta_ssid = "MyNetwork";
wifi_config.sta_password = "password123";
wifi_config.enable_ap_fallback = true;
wifi_config.sta_timeout_sec = 30;

// Initialize WiFi
esp_err_t err = wifi_subsystem.Initialize(wifi_config);
if (err != ESP_OK) {
    ESP_LOGE("App", "WiFi init failed: %s", esp_err_to_name(err));
}
```

### 2. Main Loop Monitoring

```cpp
void Run() {
    while (true) {
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        wifi_subsystem.Tick(now_ms);

        // Check if ready for HTTP server
        if (wifi_subsystem.GetStatus().ready) {
            // Start HTTP server (once)
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

### 3. Status Queries

```cpp
wifi_subsystem::WiFiStatus status = wifi_subsystem.GetStatus();

switch (status.mode) {
    case wifi_subsystem::WiFiMode::kStaConnected:
        printf("Connected to WiFi: IP=%s, RSSI=%d dBm\n",
               status.ip_address, status.rssi_dbm);
        break;
    case wifi_subsystem::WiFiMode::kApActive:
        printf("AP mode active: IP=%s, Clients=%u\n",
               status.ip_address, status.clients);
        break;
    case wifi_subsystem::WiFiMode::kStaConnecting:
        printf("Connecting to WiFi...\n");
        break;
    default:
        break;
}
```

### 4. Runtime Reconfiguration

```cpp
// Update WiFi config via console or web UI
wifi_config.sta_ssid = "NewNetwork";
wifi_config.sta_password = "newpass";
wifi_subsystem.SetConfig(wifi_config);

// Reconnect with new credentials
esp_err_t err = wifi_subsystem.Reconnect();
if (err != ESP_OK) {
    ESP_LOGE("App", "Reconnect failed: %s", esp_err_to_name(err));
}

// Save to NVS
config_storage.Save(device_config);
```

## Configuration Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `sta_ssid` | `char[32]` | `""` | Station mode SSID (empty = skip STA) |
| `sta_password` | `char[64]` | `""` | Station mode password |
| `ap_ssid` | `char[32]` | `"Keyer-AP"` | Access Point SSID |
| `ap_password` | `char[64]` | `""` | AP password (empty = open network) |
| `enable_ap_fallback` | `bool` | `true` | Enable AP mode on STA failure |
| `sta_timeout_sec` | `uint16_t` | `30` | STA connection timeout before fallback |

## State Machine

```
┌──────┐
│ Idle │
└───┬──┘
    │ Initialize()
    ▼
┌───────────────┐
│ StaConnecting │◄─────┐
└────┬─────┬────┘      │
     │     │            │ Retry
     │     └────────────┘
     │ Got IP
     ▼
┌──────────────┐   Disconnect
│ StaConnected │───────────┐
└──────────────┘           │
                           ▼
     Timeout/Failed   ┌────────────┐
     ─────────────────►│  ApActive  │
                       └────────────┘
```

## Console Integration

WiFi subsystem exposes console commands via `ConsoleCommandHandler`:

```
wifi status                         # Show current WiFi status
wifi sta <ssid> <password>          # Configure STA credentials
wifi ap <ssid> <password>           # Configure AP credentials
wifi connect                        # Force STA connection
wifi disconnect                     # Disconnect from STA
wifi save                           # Save WiFi config to NVS
wifi reload                         # Reload WiFi config from NVS
```

## Dependencies

- ESP-IDF `esp_wifi` (WiFi driver)
- ESP-IDF `esp_netif` (Network interface abstraction)
- ESP-IDF `esp_event` (Event loop)
- ESP-IDF `nvs_flash` (Configuration storage)
- `config` component (WiFiConfig structure)

## Memory Usage

- **Heap**: ~20 KB (WiFi stack + event loop)
- **Stack**: Event handlers run in ESP-IDF event task (4 KB)
- **Flash**: ~150 KB (WiFi PHY, RF calibration data)

## Thread Safety

| Method | Thread-Safe | Notes |
|--------|-------------|-------|
| `Initialize()` | No | Call from main task only |
| `Deinitialize()` | No | Call from main task only |
| `Tick()` | No | Call from main loop (not ISR-safe) |
| `GetStatus()` | **Yes** | Atomic state access |
| `SetConfig()` | No | Requires external synchronization |
| `Reconnect()` | No | Call from main task only |

## Known Limitations

1. **Single WiFi Mode**: STA and AP cannot run simultaneously (ESP32-S3 limitation)
2. **No WiFi Scan**: Future enhancement to list available networks
3. **No Static IP**: Uses DHCP in STA mode, fixed 192.168.4.1 in AP mode
4. **No Enterprise WiFi**: Only supports WPA2-PSK (not WPA2-Enterprise)

## Testing

Host-side unit tests not yet implemented (requires ESP-IDF stubs for WiFi APIs).

Hardware testing checklist:
- [ ] STA connection to WPA2-PSK network
- [ ] STA timeout and AP fallback
- [ ] AP mode with open network
- [ ] AP mode with WPA2-PSK password
- [ ] Reconnection after disconnect
- [ ] Console WiFi commands
- [ ] NVS persistence across reboots

## Future Enhancements

- WiFi scan API for network discovery
- Static IP configuration support
- WPS (Wi-Fi Protected Setup) provisioning
- SmartConfig/BLE provisioning
- WiFi signal strength LED indicator
- Automatic channel selection for AP mode
- Captive portal for AP mode configuration
