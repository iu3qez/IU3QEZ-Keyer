# Keyer QRS2HST - Web UI REST/WebSocket API Specification

**Version**: 1.0
**Last Updated**: 2025-10-26
**Status**: Draft (Task 5.4.1)

## Overview

This document defines the REST API and WebSocket protocol for the Keyer QRS2HST Web UI configuration interface. The API is built on top of the Parameter Metadata System, providing a unified, type-safe interface for all device configuration.

### Design Principles

1. **Parameter-driven**: All configuration uses the ParameterRegistry for validation and type safety
2. **RESTful**: Standard HTTP methods (GET, POST) with JSON responses
3. **Real-time updates**: WebSocket for live status broadcasting (WPM, mode, decoded text)
4. **Self-documenting**: Schema export with widget hints enables auto-generated UI
5. **CORS-enabled**: Access-Control-Allow-Origin headers for development
6. **Secure by default**: Passwords masked in responses, HTTPS recommended for production

### Base URL

- **Development (AP mode)**: `http://192.168.4.1`
- **Production (STA mode)**: `http://<device-ip>` (obtained via mDNS or DHCP)

### Content Types

- Request: `application/json` or `application/x-www-form-urlencoded`
- Response: `application/json`
- WebSocket: `text` (JSON strings)

---

## REST API Endpoints

### 1. Configuration Schema

#### GET /api/config/schema

Returns JSON schema for all visible parameters with widget hints for Web UI auto-generation.

**Query Parameters**: None

**Response**: `200 OK`

```json
{
  "parameters": [
    {
      "name": "audio.freq",
      "type": "int",
      "widget": "number_input",
      "min": 100,
      "max": 2000,
      "unit": "Hz",
      "description": "Sidetone frequency"
    },
    {
      "name": "keying.preset",
      "type": "enum",
      "widget": "dropdown",
      "values": [
        {"name": "V0", "description": "SuperKeyer with dot+dash memory"},
        {"name": "V1", "description": "SuperKeyer with dot-only memory"},
        {"name": "V3", "description": "Accukeyer with dot+dash memory"},
        {"name": "MANUAL", "description": "Manual configuration"}
      ],
      "description": "Iambic keying preset"
    },
    {
      "name": "keying.window_open",
      "type": "float",
      "widget": "slider",
      "min": 0.0,
      "max": 100.0,
      "precision": 1,
      "unit": "%",
      "description": "Memory window open threshold"
    },
    {
      "name": "audio.enabled",
      "type": "bool",
      "widget": "checkbox",
      "true": "on",
      "false": "off",
      "description": "Sidetone enabled"
    },
    {
      "name": "wifi.sta_password",
      "type": "string",
      "widget": "text_input",
      "min_length": 0,
      "max_length": 63,
      "masked": true,
      "description": "Station password"
    }
  ]
}
```

**Notes**:
- Only visible parameters are included (respects `IsVisible()` conditions)
- Widget hints: `number_input`, `slider`, `dropdown`, `checkbox`, `text_input`
- Conditional visibility: `keying.window_open` only visible when `keying.preset == MANUAL`
- Masked fields: `masked: true` indicates password/secret fields

**Implementation**:
```cpp
// Handler pseudocode
auto* ctx = static_cast<HandlerContext*>(req->user_ctx);
std::string schema = ctx->param_registry->ExportJsonSchema(*ctx->config);
httpd_resp_set_type(req, "application/json");
httpd_resp_sendstr(req, schema.c_str());
```

---

### 2. Get Current Configuration

#### GET /api/config

Returns current configuration values for all parameters organized by subsystem.

**Query Parameters**:
- `subsystem` (optional): Filter by subsystem prefix (e.g., `audio`, `keying`, `wifi`)

**Response**: `200 OK`

```json
{
  "general": {
    "callsign": "N0CALL"
  },
  "audio": {
    "freq": 700,
    "volume": 50,
    "fade_in": 5,
    "fade_out": 10,
    "enabled": false
  },
  "keying": {
    "wpm": 20,
    "preset": "V3",
    "late": "consider"
  },
  "wifi": {
    "sta_ssid": "MyNetwork",
    "sta_password": "****",
    "ap_ssid": "Keyer-AP",
    "ap_password": "****",
    "fallback": true,
    "sta_timeout": 30
  },
  "hardware": {
    "dit_gpio": 1,
    "dah_gpio": 2,
    "key_gpio": 3,
    "paddles_active_low": true,
    "use_pullups": true,
    "use_pulldowns": false
  }
}
```

**Example with subsystem filter**:

Request: `GET /api/config?subsystem=audio`

Response:
```json
{
  "audio": {
    "freq": 700,
    "volume": 50,
    "fade_in": 5,
    "fade_out": 10,
    "enabled": false
  }
}
```

**Notes**:
- Passwords/secrets masked with `****` (respects `StringParameter` mask token)
- Only visible parameters included
- Values formatted using `Parameter::GetCurrentValue()`

---

### 3. Update Parameter Value

#### POST /api/config/{subsystem}/{param}

Update a single configuration parameter.

**Path Parameters**:
- `{subsystem}`: Subsystem name (e.g., `audio`, `keying`, `wifi`)
- `{param}`: Parameter name without subsystem prefix (e.g., `freq`, `wpm`, `preset`)

**Request Body** (JSON):
```json
{
  "value": "700"
}
```

**Response**: `200 OK` (success)

```json
{
  "success": true,
  "message": "OK audio.freq=700Hz"
}
```

**Response**: `400 Bad Request` (validation error)

```json
{
  "success": false,
  "message": "ERR Value must be between 100 and 2000"
}
```

**Response**: `404 Not Found` (parameter not found or not visible)

```json
{
  "success": false,
  "message": "ERR Parameter not found: audio.invalid"
}
```

**Examples**:

1. Set sidetone frequency:
   ```
   POST /api/config/audio/freq
   {"value": "800"}
   ```

2. Change keying preset:
   ```
   POST /api/config/keying/preset
   {"value": "V6"}
   ```

3. Enable sidetone:
   ```
   POST /api/config/audio/enabled
   {"value": "on"}
   ```

**Implementation**:
```cpp
// Handler pseudocode
const char* param_name = build_full_name(subsystem, param);
Parameter* p = ctx->param_registry->Find(param_name);
if (!p || !p->IsVisible(*ctx->config)) {
  return send_error(404, "Parameter not found");
}

std::string result_msg;
bool ok = p->Execute(value_str, *ctx->config, &result_msg);
return send_json(ok ? 200 : 400, result_msg);
```

**Notes**:
- Changes are applied to `DeviceConfig` in-memory immediately
- **Not persisted to NVS** until `/api/config/save` is called
- Validation performed by `Parameter::Execute()`
- Result message format: `OK param=value` or `ERR error message`

---

### 4. Save Configuration

#### POST /api/config/save

Persist current in-memory configuration to NVS (non-volatile storage).

**Request Body**: None (or empty JSON `{}`)

**Response**: `200 OK`

```json
{
  "success": true,
  "message": "Configuration saved to NVS"
}
```

**Response**: `500 Internal Server Error`

```json
{
  "success": false,
  "message": "Failed to save configuration: ESP_ERR_NVS_NOT_ENOUGH_SPACE"
}
```

**Implementation**:
```cpp
esp_err_t err = ctx->storage->Save(*ctx->config);
if (err != ESP_OK) {
  return send_error(500, "Failed to save: " + esp_err_to_name(err));
}
return send_json(200, "Configuration saved to NVS");
```

**Notes**:
- All subsystems saved atomically
- Version field updated automatically
- Backup strategy: See `Storage::Backup()` / `Storage::Restore()`

---

### 5. Reboot Device

#### POST /api/system/reboot

Reboot the device (required for hardware config changes).

**Request Body**: None

**Response**: `200 OK`

```json
{
  "success": true,
  "message": "Rebooting in 2 seconds..."
}
```

**Implementation**:
```cpp
esp_restart();  // Or use FreeRTOS task with delay
```

**Notes**:
- Required after changing hardware parameters (GPIO pins, I2C/I2S config)
- Client should reconnect after ~5 seconds

---

### 6. WiFi Configuration (Existing - Task 5.4.0.6)

#### GET /api/config/wifi

Returns current WiFi configuration (SSID only, password masked).

**Response**: `200 OK`

```json
{
  "ssid": "MyNetwork",
  "fallback": true
}
```

#### POST /api/config/wifi

Update WiFi configuration and trigger reconnect.

**Request Body** (URL-encoded form):
```
ssid=MyNetwork&password=MyPassword&fallback=1
```

**Response**: `200 OK`

```json
{
  "success": true,
  "message": "Configuration saved"
}
```

**Notes**:
- Automatically saves to NVS and triggers `WiFiSubsystem::Reconnect()`
- Fallback: `1` = enabled, `0` = disabled

---

### 7. System Status

#### GET /api/status

Returns current system status (WiFi, keying mode, WPM, etc.).

**Response**: `200 OK`

```json
{
  "wifi": {
    "mode": "Connected (STA)",
    "ip": "192.168.1.100",
    "rssi": -45,
    "clients": 0,
    "ready": true
  },
  "keying": {
    "preset": "V3",
    "wpm": 20,
    "active": false,
    "last_element": "dit"
  },
  "audio": {
    "enabled": true,
    "playing": false
  },
  "uptime_ms": 3600000
}
```

**Notes**:
- Polled by Web UI every 5 seconds for status bar updates
- For real-time updates, use WebSocket (see below)

---

### 8. Timeline Export (Optional - Task 4.4)

#### GET /api/timeline

Export timeline events for diagnostics.

**Query Parameters**:
- `format`: `json` or `csv` (default: `json`)
- `limit`: Maximum number of events (default: 1000)
- `after`: Timestamp (µs) - return only events after this time

**Response**: `200 OK` (JSON format)

```json
{
  "events": [
    {
      "timestamp_us": 1234567890,
      "type": "paddle_dit_press",
      "duration_us": 120000
    },
    {
      "timestamp_us": 1234687890,
      "type": "paddle_dit_release"
    },
    {
      "timestamp_us": 1234807890,
      "type": "symbol_dit",
      "confidence": 0.98
    }
  ]
}
```

**Response**: `200 OK` (CSV format)

```csv
timestamp_us,type,duration_us,confidence
1234567890,paddle_dit_press,120000,
1234687890,paddle_dit_release,,
1234807890,symbol_dit,,0.98
```

**Notes**:
- Timeline events logged by all subsystems (paddle HAL, keying engine, decoder)
- Ring buffer implementation (oldest events overwritten)
- High-frequency logging: may impact performance if polled too often

---

## WebSocket Protocol

### Endpoint: WS /api/ws

Real-time bidirectional communication for status updates and live data.

**Connection**: Upgrade HTTP to WebSocket

```
GET /api/ws HTTP/1.1
Host: 192.168.4.1
Upgrade: websocket
Connection: Upgrade
```

### Server → Client Messages

#### 1. Status Update

Sent every 100ms when keying activity detected, or every 5s when idle.

```json
{
  "type": "status",
  "data": {
    "wpm": 20,
    "mode": "V3",
    "active": true,
    "last_element": "dah",
    "audio_playing": true
  }
}
```

#### 2. Decoded Text

Sent when CW decoder produces new text (Task 4.3).

```json
{
  "type": "decoded",
  "data": {
    "text": "CQ CQ DE N0CALL K",
    "confidence": 0.97,
    "wpm_detected": 22
  }
}
```

#### 3. Configuration Changed

Broadcast when another client updates configuration.

```json
{
  "type": "config_changed",
  "data": {
    "subsystem": "audio",
    "parameter": "freq",
    "value": "800"
  }
}
```

#### 4. Error/Warning

System errors or warnings.

```json
{
  "type": "error",
  "data": {
    "message": "Sidetone buffer underrun detected"
  }
}
```

### Client → Server Messages

#### 1. Subscribe to Updates

Request specific update types.

```json
{
  "type": "subscribe",
  "channels": ["status", "decoded", "config"]
}
```

#### 2. Ping/Keepalive

Keep connection alive.

```json
{
  "type": "ping"
}
```

**Server Response**:
```json
{
  "type": "pong"
}
```

**Notes**:
- WebSocket implemented using ESP-IDF `httpd_ws_*` API
- Maximum 4 concurrent WebSocket connections
- Automatic disconnect after 60s idle (no pong received)

---

## Error Handling

### HTTP Status Codes

- `200 OK`: Success
- `400 Bad Request`: Validation error, malformed JSON
- `404 Not Found`: Parameter/endpoint not found
- `408 Request Timeout`: Client timeout (POST body not received)
- `500 Internal Server Error`: NVS failure, system error

### Error Response Format

All errors return JSON with `success: false` and descriptive message:

```json
{
  "success": false,
  "message": "ERR Value must be between 100 and 2000"
}
```

---

## Authentication & Security

### Current Implementation (v1.0)

- **No authentication**: Open access when connected to AP or STA network
- **Password masking**: Passwords never returned in plaintext (`****`)
- **CORS enabled**: `Access-Control-Allow-Origin: *` for development

### Future Considerations

- HTTP Basic Auth for production deployments
- HTTPS/TLS (requires certificate management on ESP32)
- API token-based authentication for remote clients
- Rate limiting for brute-force protection

---

## Implementation Notes

### Handler Registration Pattern

```cpp
// In HttpServer::Initialize()

// Existing WiFi endpoint (Task 5.4.0.6)
httpd_register_uri_handler(server_, &(httpd_uri_t){
  .uri = "/api/config/wifi",
  .method = HTTP_GET,
  .handler = HandleGetWiFiConfig,
  .user_ctx = &context_
});

// New schema endpoint (Task 5.4.2)
httpd_register_uri_handler(server_, &(httpd_uri_t){
  .uri = "/api/config/schema",
  .method = HTTP_GET,
  .handler = HandleGetSchema,
  .user_ctx = &context_
});

// New parameter update endpoint (Task 5.4.2)
httpd_register_uri_handler(server_, &(httpd_uri_t){
  .uri = "/api/config/*",  // Wildcard for subsystem/param
  .method = HTTP_POST,
  .handler = HandlePostParameter,
  .user_ctx = &context_
});
```

### Context Structure Extension

```cpp
struct HandlerContext {
  config::DeviceConfig* config;
  wifi_subsystem::WiFiSubsystem* wifi;
  config::Storage* storage;
  config::ParameterRegistry* param_registry;  // Added for Task 5.4.2
};
```

### WebSocket Handler Skeleton

```cpp
static esp_err_t HandleWebSocket(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    // Upgrade to WebSocket
    return ESP_OK;
  }

  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;

  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) return ret;

  // Allocate buffer and receive payload
  // Parse JSON, route to handler
  // Send response via httpd_ws_send_frame()
}
```

---

## Testing Strategy

### Manual Testing

1. **Schema export**: `curl http://192.168.4.1/api/config/schema | jq`
2. **Get config**: `curl http://192.168.4.1/api/config?subsystem=audio | jq`
3. **Update param**: `curl -X POST -H "Content-Type: application/json" -d '{"value":"800"}' http://192.168.4.1/api/config/audio/freq`
4. **Save config**: `curl -X POST http://192.168.4.1/api/config/save`
5. **WebSocket**: Use `wscat` or browser console

### Automated Testing (Future - Task 7.x)

- Python test client with `requests` and `websocket-client`
- Validate schema against expected parameter count
- Test parameter validation (invalid ranges, types)
- Stress test: rapid parameter updates
- WebSocket message ordering and latency

---

## API Versioning

Current version: **v1.0**

Future versions may introduce:
- `/api/v2/config/schema` - versioned endpoints
- Backwards compatibility for older Web UI clients
- Deprecation warnings in responses

---

## Changelog

### v1.0 (2025-10-26)
- Initial API specification (Task 5.4.1)
- WiFi endpoints already implemented (Task 5.4.0.6)
- Schema export, parameter update, save config defined
- WebSocket protocol specified for future implementation

---

**End of API Specification**
