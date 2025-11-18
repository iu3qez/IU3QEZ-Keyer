# Remote CW Keying Reference

## Overview

The Remote CW Keying feature enables real-time, low-latency transmission of paddle keying over TCP/IP networks using the **CWNet protocol** (DL4YHF implementation). This allows you to operate a remote radio's CW transmitter from a local paddle connected to the keyer.

### Key Features

- **Real-time keying transmission** with automatic PTT management
- **Network latency compensation** for PTT timing (base + measured RTT)
- **Bidirectional modes**: Client (send keying) and Server (receive keying)
- **Connection monitoring** with automatic reconnection
- **Web UI status page** for real-time monitoring and control
- **Console commands** for headless operation

### Use Cases

1. **Remote Station Operation**: Key a remote radio from your local paddle
2. **Multi-Operator Setups**: Share keying between multiple stations
3. **Emergency/Portable**: Access home station keyer remotely
4. **Contest Networking**: Coordinate CW operations across multiple locations

---

## Architecture

The keyer implements both **client** and **server** roles:

### Client Mode (Default Use Case)

The keyer acts as a **client** that:
1. Connects to a remote CWNet server (e.g., another keyer or DL4YHF's CWNetKeyer)
2. Sends local paddle events (key down/up with timestamps)
3. Manages PTT with dynamic tail timing (200ms base + network latency)
4. Measures round-trip latency via 3-way PING protocol
5. Handles CMD_PRINT messages from server (debug/status)

**Client State Machine:**
```
Idle → Resolving → Connecting → Handshake → Connected
  ↑                                             ↓
  └──────────────── Error ←────────────────────┘
                (auto-reconnect if enabled)
```

### Server Mode (Receiving Keying)

The keyer acts as a **server** that:
1. Listens on a TCP port (default 7355)
2. Accepts a **single client** connection (mono-client)
3. Receives MORSE frames (key down/up events) from client
4. Activates TX HAL output to key local radio
5. Manages PTT tail timeout (configurable, default 200ms)
6. Responds to PING requests for latency measurement

**Server State Machine:**
```
Idle → Listening → Handshake → Connected
  ↑                               ↓
  └──────────── Error ←───────────┘
```

---

## Remote Audio Streaming (RX Mode)

The keyer now supports **remote CW audio reception** from the server during RX mode (when NOT transmitting locally). This allows you to hear the remote station's CW audio in your headphones/speaker.

### How It Works

**Automatic TX/RX Switching:**
- **TX Mode** (paddle pressed): Local tone generator produces sidetone
- **RX Mode** (paddle idle > 200ms): Remote audio stream plays through codec
- **Switching**: Seamless transition based on paddle state + PTT timeout

**Audio Pipeline:**
```
Server (48kHz) → A-Law Compression (8 kHz) → CMD_AUDIO Frames → Network
                                                                     ↓
Client: A-Law Decoder → 8→16kHz Upsampling → Ring Buffer → ES8311 Codec → Speaker
```

**Technical Details:**
- **Codec**: A-Law (ITU-T G.711) - 8-bit compressed PCM
- **Network Sample Rate**: 8 kHz (from server)
- **Local Codec Rate**: 16 kHz (upsampled via linear interpolation)
- **Buffering**: 160ms ring buffer (1280 samples @ 8kHz)
- **Latency**: ~40ms min buffering + network RTT
- **Underrun Handling**: Automatic silence on buffer underrun, resumes when refilled

### Configuration

**No additional parameters required!** Audio streaming works automatically when:
1. Remote client is connected (`remote.enabled=true`)
2. Server sends CMD_AUDIO frames (DL4YHF CWNetKeyer with audio enabled)
3. Paddle is idle (not transmitting)

### Usage

**Normal Operation:**
```bash
# Connect to server with audio streaming
> set remote server_host 192.168.1.100
> remote start

# Status check
> remote status
State:   Connected
Latency: 42 ms (round-trip)

# Listen for remote station CW:
# - Keep paddle idle → you hear remote audio
# - Press paddle → local sidetone (TX mode)
# - Release paddle (wait 200ms) → back to remote audio (RX mode)
```

**Monitoring Audio Stats:**

Check for underruns or buffer issues:
```bash
# Serial console log messages:
I (xxx) SidetoneService: Switching audio mode: ToneGenerator → StreamPlayer
I (xxx) RemoteCwClient: Received CMD_AUDIO frame (payload_size=64)
W (xxx) AudioStreamPlayer: Underrun detected (available=240, requested=256)
```

### Troubleshooting Audio

**No Remote Audio Heard:**
- ✅ Verify remote client is connected (`remote status` shows "Connected")
- ✅ Ensure paddle is idle (not transmitting)
- ✅ Check server is sending CMD_AUDIO frames (check logs)
- ✅ Verify local codec volume (`set audio volume <0-100>`)
- ✅ Check speaker/headphone connection

**Audio Choppy or Cutting Out:**
- **Cause**: Network packet loss or high jitter
- **Fix**: Improve WiFi signal strength, reduce network congestion
- **Symptom**: Log shows frequent "Underrun detected" warnings

**Audio Delayed (High Latency):**
- **Expected**: ~40-80ms buffering + network RTT
- **Acceptable**: <150ms total latency for monitoring
- **If > 200ms**: Check network latency (`remote status` shows RTT)

**Audio Continues During TX:**
- **Bug**: Should switch to local sidetone immediately
- **Debug**: Check logs for "Switching audio mode" messages
- **Workaround**: Restart remote connection

---

## CWNet Protocol Summary

The keyer implements the **DL4YHF CWNet protocol** (binary, TCP-based):

### Frame Format

```
[Command Byte] [Optional Length] [Payload...]

Command Byte:
  Bits 7-6: Block length encoding (00=no payload, 01=1 byte, 10=2 bytes, 11=variable)
  Bits 5-0: Command type (0x00-0x3F)
```

### Supported Commands

| Command | Direction | Purpose | Status |
|---------|-----------|---------|--------|
| **CMD_CONNECT** (0x00) | Client→Server | Initial handshake with callsign | ✅ Implemented |
| **CMD_DISCONNECT** (0x01) | Bidirectional | Graceful connection close | ✅ Implemented |
| **CMD_MORSE** (0x02) | Client→Server | Key down/up events with 7-bit timestamps | ✅ Implemented |
| **CMD_PING_REQUEST** (0x03) | Client→Server | Latency measurement (3-way) | ✅ Implemented |
| **CMD_PING_RESPONSE_1** (0x04) | Server→Client | First PING echo | ✅ Implemented |
| **CMD_PING_RESPONSE_2** (0x3D) | Client→Server | Second PING echo | ✅ Implemented |
| **CMD_PRINT** (0x3E) | Server→Client | Text message display | ✅ Implemented |
| **CMD_AUDIO** (0x11) | Server→Client | Remote CW audio stream (A-Law codec) | ✅ Implemented |
| **CMD_TX_INFO** (0x05) | - | TX parameter info | 🚧 Stub |
| **CMD_RIGCTLD** (0x06) | - | Rig control commands | 🚧 Stub |
| **CMD_TUNNEL_1/2/3** (0x38-0x3A) | - | Generic tunneling | 🚧 Stub |

**Note:** MORSE frames are **unidirectional** (client→server only). The server does not send MORSE events back to the client.

### Timestamp Encoding (7-bit Non-Linear)

MORSE frames use compressed timestamps for efficient transmission:
- **0-31**: 1ms resolution (0-31ms)
- **32-156**: 4ms resolution (32-156ms)
- **157-1165**: 16ms resolution (157-1165ms)

---

## Configuration

### Web UI Configuration

1. Navigate to the **configuration page** (`http://<keyer-ip>/`)
2. Expand the **"Remote Keying"** section
3. Configure client settings:
   - **remote.enabled**: Enable/disable remote client (`true`/`false`)
   - **remote.server_host**: Server hostname/IP (e.g., `192.168.1.100` or `cw.example.com`)
   - **remote.server_port**: Server port (default: `7355`)
   - **remote.auto_reconnect**: Auto-reconnect on disconnect (`true`/`false`)
   - **remote.ptt_tail_ms**: Base PTT tail in milliseconds (default: `200`)
4. Configure server settings (if receiving keying):
   - **server.enabled**: Enable/disable server mode (`true`/`false`)
   - **server.listen_port**: TCP port to listen on (default: `7355`)
   - **server.ptt_tail_ms**: PTT tail for received keying (default: `200`)
5. Click **"Save to NVS"** to persist changes

**Important:** Client and server can run **simultaneously** (e.g., relay/repeater mode).

### Console Configuration

Use the `set` command to configure parameters:

```bash
# Client configuration
> set remote enabled true
> set remote server_host 192.168.1.100
> set remote server_port 7355
> set remote auto_reconnect true
> set remote ptt_tail_ms 200

# Server configuration
> set server enabled true
> set server listen_port 7355
> set server ptt_tail_ms 200

# Display current settings
> show remote
REMOTE enabled=true server_host=192.168.1.100 server_port=7355 auto_reconnect=true ptt_tail_ms=200

> show server
SERVER enabled=true listen_port=7355 ptt_tail_ms=200

# Save configuration
> save
Configuration saved to NVS
```

---

## Usage

### Starting/Stopping Client (Console)

```bash
# Start client connection
> remote start
Remote client started (connecting...)

# Check connection status
> remote status

Remote CW Client Status:
------------------------
State:   Connected
Latency: 42 ms (round-trip)

# Stop client
> remote stop
Remote client stopped
```

### Starting/Stopping Server (Console)

```bash
# Start server
> server start
Server started (listening for connections...)

# Check server status
> server status

Remote CW Server Status:
------------------------
State:   Client connected (receiving keying)

# Stop server
> server stop
Server stopped
```

### Web UI Monitoring

1. Navigate to **Remote Keying Status** page (`http://<keyer-ip>/html/remote.html`)
2. The page displays:
   - **Client Status Card**:
     - Connection state (Idle, Resolving, Connecting, Handshake, Connected, Error)
     - Server address and port
     - Round-trip latency (when connected)
     - Dynamic PTT tail (base + latency)
     - Start/Stop buttons
   - **Server Status Card**:
     - Server state (Idle, Listening, Handshake, Connected, Error)
     - Listen port
     - Connected client IP address
     - PTT tail setting
     - Start/Stop buttons
   - **Configuration Summary**:
     - All remote/server parameter values
     - Enable/disable states
     - Auto-reconnect setting

3. **Real-time updates**: The page polls `/api/remote/status` every 1 second
4. **Control buttons**: Click Start/Stop to control client/server runtime

---

## PTT Management

### Client PTT Logic

When you press the paddle:
1. **Immediate PTT activation**: PTT output goes active on first key-down
2. **Key events sent to server**: MORSE frames transmitted with timestamps
3. **Dynamic PTT tail**: PTT remains active for `base_ms + latency_ms` after last key-up
   - Example: Base 200ms + 42ms latency = **242ms PTT tail**
   - Prevents clipping on high-latency networks
4. **Automatic release**: PTT drops after tail timeout expires

### Server PTT Logic

When receiving keying from client:
1. **TX HAL activation**: Key-down events activate transmitter output
2. **PTT tail timeout**: PTT remains active for `server.ptt_tail_ms` after last key-up
3. **Single-client limit**: Only one client can connect at a time

**Note:** PTT tail timing is **critical** for preventing clipped transmissions. Increase `ptt_tail_ms` for high-latency or unreliable networks.

---

## Testing and Debugging

### Local Loopback Test

Test client/server on the same keyer:

```bash
# Terminal 1: Start server
> set server enabled true
> server start
Server started (listening for connections...)

# Terminal 2: Start client
> set remote enabled true
> set remote server_host 127.0.0.1
> set remote server_port 7355
> remote start
Remote client started (connecting...)

# Check status
> remote status
State:   Connected
Latency: 2 ms (round-trip)

# Paddle input should now be relayed through network to TX output
```

### Network Testing with Another Keyer

Setup two keyers (A and B) on the same network:

**Keyer A (Server):**
```bash
> set server enabled true
> server start
Server started (listening for connections...)
```

**Keyer B (Client):**
```bash
> set remote enabled true
> set remote server_host 192.168.1.100  # IP of Keyer A
> remote start
Remote client started (connecting...)
> remote status
State:   Connected
Latency: 15 ms (round-trip)
```

Press paddle on **Keyer B** → TX output activates on **Keyer A**.

### Testing with DL4YHF CWNetKeyer

The keyer is compatible with DL4YHF's original CWNetKeyer implementation:

1. **Run CWNetKeyer as server** (Windows/Linux):
   ```bash
   CWNetKeyer.exe -server -port 7355
   ```

2. **Configure keyer as client**:
   ```bash
   > set remote server_host <CWNetKeyer_IP>
   > remote start
   ```

3. Paddle input on keyer → CW keying on CWNetKeyer

### Debug Commands

Enable verbose logging:
```bash
> debug verbose
Log level set to: verbose
```

Monitor connection events:
```bash
I (12345) remote_client: State: Idle → Resolving
I (12346) remote_client: DNS resolved: 192.168.1.100
I (12347) remote_client: State: Resolving → Connecting
I (12348) remote_client: TCP socket connected
I (12349) remote_client: State: Connecting → Handshake
I (12350) remote_client: Sent CMD_CONNECT: IU3QEZ
I (12351) remote_client: State: Handshake → Connected
I (12352) remote_client: PING RTT: 42 ms
```

---

## Troubleshooting

### Client Won't Connect

**Symptoms:** Client stuck in "Resolving" or "Connecting" state

**Checks:**
1. Verify server hostname/IP is correct:
   ```bash
   > show remote server_host
   ```
2. Verify server is reachable (ping from another device)
3. Check firewall rules on server (allow TCP port 7355)
4. Verify server is actually running:
   ```bash
   # On server keyer
   > server status
   State:   Listening (waiting for client)
   ```
5. Check network connectivity (same subnet, VPN active, etc.)

**Debug:**
```bash
> debug debug
> remote start
# Check logs for DNS/TCP errors
```

### Client Error State

**Symptoms:** Client shows "Error (will retry if auto_reconnect enabled)"

**Causes:**
- Server unreachable or rejected connection
- Network timeout during handshake
- Server crashed or disconnected

**Fix:**
1. Check server status
2. Verify `auto_reconnect` is enabled:
   ```bash
   > set remote auto_reconnect true
   ```
3. Restart client:
   ```bash
   > remote stop
   > remote start
   ```

### High Latency

**Symptoms:** Latency > 100ms on local network

**Checks:**
1. Verify both devices are on same LAN (not routed through internet)
2. Check for network congestion (speed test)
3. Disable WiFi power save mode (if using WiFi)
4. Use Ethernet instead of WiFi if possible

**Workaround:** Increase PTT tail to prevent clipping:
```bash
> set remote ptt_tail_ms 400  # 400ms + latency
```

### PTT Cuts Off Morse Code

**Symptoms:** Last dits/dahs are clipped on transmitted signal

**Cause:** Insufficient PTT tail for network latency

**Fix:** Increase PTT tail:
```bash
# Client side
> set remote ptt_tail_ms 300
> save

# Server side (if receiving keying)
> set server ptt_tail_ms 300
> save
```

### Server Rejects Connection

**Symptoms:** Client connects briefly then disconnects

**Cause:** Server already has a client connected (mono-client design)

**Fix:**
1. Stop existing client connection to server
2. Restart server:
   ```bash
   > server stop
   > server start
   ```

### Missing Keying Events

**Symptoms:** Paddle input doesn't produce remote keying

**Checks:**
1. Verify client is in "Connected" state:
   ```bash
   > remote status
   State:   Connected  # Must show Connected
   ```
2. Verify paddle is working locally:
   ```bash
   > keying status
   # Send some paddle input, check for events
   ```
3. Check server logs for received MORSE frames (if accessible)

---

## Security Considerations

### Network Security

- **No encryption**: CWNet protocol is **plaintext TCP** (no TLS/SSL)
- **No authentication**: Any client can connect if network is reachable
- **Recommended deployment**:
  - Use on **private/trusted networks only** (home LAN, VPN)
  - **Firewall rules**: Restrict port 7355 to known IP ranges
  - **VPN**: Use WireGuard/OpenVPN for remote access

### Port Forwarding Warning

**Do NOT expose port 7355 to the public internet** without additional security:
- No password/authentication
- No rate limiting
- Vulnerable to replay attacks and DoS

---

## API Reference (Web UI)

The Web UI communicates with the following REST API endpoints:

### GET `/api/remote/status`

Returns current client/server status and configuration.

**Response:**
```json
{
  "client": {
    "state": 4,               // 0=Idle, 1=Resolving, 2=Connecting, 3=Handshake, 4=Connected, 5=Error
    "server_host": "192.168.1.100",
    "server_port": 7355,
    "latency_ms": 42,
    "ptt_tail_base_ms": 200
  },
  "server": {
    "state": 3,               // 0=Idle, 1=Listening, 2=Handshake, 3=Connected, 4=Error
    "listen_port": 7355,
    "client_ip": "192.168.1.50",
    "ptt_tail_ms": 200
  },
  "config": {
    "client_enabled": true,
    "client_server_host": "192.168.1.100",
    "client_server_port": 7355,
    "client_auto_reconnect": true,
    "server_enabled": true,
    "server_listen_port": 7355
  }
}
```

### POST `/api/remote/client/start`

Starts the remote client connection.

**Response:**
```json
{
  "success": true,
  "message": "Client started"
}
```

### POST `/api/remote/client/stop`

Stops the remote client connection.

**Response:**
```json
{
  "success": true,
  "message": "Client stopped"
}
```

### POST `/api/remote/server/start`

Starts the remote CW server.

**Response:**
```json
{
  "success": true,
  "message": "Server started"
}
```

### POST `/api/remote/server/stop`

Stops the remote CW server.

**Response:**
```json
{
  "success": true,
  "message": "Server stopped"
}
```

---

## Protocol Implementation Details

### 3-Way PING Latency Measurement

Client initiates latency measurement every 5 seconds when connected:

```
Client → Server: CMD_PING_REQUEST (timestamp T1)
Server → Client: CMD_PING_RESPONSE_1 (echo T1)
Client → Server: CMD_PING_RESPONSE_2 (echo T1)

RTT = (current_time - T1) / 2
```

This 3-way handshake provides accurate round-trip latency measurement.

### MORSE Frame Encoding

Each paddle event is encoded as a MORSE frame:

```
[0x42] [timestamp_7bit] [key_state]

0x42 = CMD_MORSE (0x02) | Block length 1 byte (0x40)
timestamp_7bit = 7-bit compressed timestamp (see encoding above)
key_state = 0x00 (key up) or 0x01 (key down)
```

Timestamps are **relative** to previous event (delta encoding).

---

## Advanced Configuration

### Simultaneous Client + Server (Relay Mode)

You can run both client and server simultaneously to create a **relay/repeater**:

```bash
# Enable both
> set remote enabled true
> set server enabled true

# Start both
> remote start
> server start
```

**Use case:** Relay keying from Client A → This Keyer (server) → forward to Remote Server

**Note:** Currently, received keying from server is **not forwarded** to client. This requires custom code.

### Custom Callsign

The client sends the **general.callsign** parameter during CMD_CONNECT handshake:

```bash
> set general callsign W1ABC
> save
> remote start
# Server receives "W1ABC" in CONNECT frame
```

---

## Future Enhancements

Planned features for future releases:

- [x] **Audio streaming** (CMD_AUDIO support) - ✅ **IMPLEMENTED** (v1.1)
- [ ] **Rig control integration** (CMD_RIGCTLD)
- [ ] **Multi-client server** (allow 2+ simultaneous clients)
- [ ] **TLS/SSL encryption** (secure CWNet protocol)
- [ ] **Authentication** (password/key-based auth)
- [ ] **Keying relay mode** (forward received keying to remote server)
- [ ] **Latency graph** (Web UI real-time latency chart)
- [ ] **Bi-directional audio** (send local audio to server)

---

## References

- **CWNet Protocol**: DL4YHF implementation (https://www.qsl.net/dl4yhf/cwnet.html)
- **Original CWNetKeyer**: Windows/Linux application for remote keying
- **ESP-IDF LwIP**: TCP socket implementation
- **3-way PING**: RFC-inspired latency measurement

---

## Appendix: State Machine Diagrams

### Client State Transitions

```
┌─────────────────────────────────────────────────────────────┐
│                      Client State Machine                    │
└─────────────────────────────────────────────────────────────┘

  Start()
    │
    ▼
┌──────┐  DNS Query   ┌───────────┐  TCP Connect  ┌────────────┐
│ Idle │─────────────→│ Resolving │──────────────→│ Connecting │
└──────┘              └───────────┘               └────────────┘
    ▲                      │                            │
    │                      │ Timeout/Error              │ Connected
    │                      ▼                            ▼
    │                  ┌───────┐                  ┌───────────┐
    │                  │ Error │←─────────────────│ Handshake │
    │                  └───────┘  Handshake Fail  └───────────┘
    │                      │                            │
    │  Stop() or          │                            │ CMD_CONNECT OK
    │  No Auto-Reconnect  │                            ▼
    │◄────────────────────┘                      ┌───────────┐
    │                                            │ Connected │
    │                                            └───────────┘
    │                                                  │
    │                     Disconnect/Error             │
    └──────────────────────────────────────────────────┘
```

### Server State Transitions

```
┌─────────────────────────────────────────────────────────────┐
│                      Server State Machine                    │
└─────────────────────────────────────────────────────────────┘

  Start()
    │
    ▼
┌──────┐  Listen()    ┌────────────┐  Client Connect  ┌───────────┐
│ Idle │─────────────→│ Listening  │─────────────────→│ Handshake │
└──────┘              └────────────┘                  └───────────┘
    ▲                      │                                │
    │                      │ Listen Error                   │ CMD_CONNECT
    │                      ▼                                ▼
    │                  ┌───────┐                      ┌───────────┐
    │                  │ Error │                      │ Connected │
    │                  └───────┘                      └───────────┘
    │                      │                                │
    │  Stop()              │                                │
    │◄─────────────────────┴────────────────────────────────┘
                              Client Disconnect/Error
```

---

**Document Version:** 1.1
**Last Updated:** 2025-11-14
**Author:** IU3QEZ Keyer Project / Claude Code
**Changelog:**
- v1.1 (2025-11-14): Added Remote Audio Streaming (RX Mode) documentation
- v1.0 (2025-10-28): Initial version
