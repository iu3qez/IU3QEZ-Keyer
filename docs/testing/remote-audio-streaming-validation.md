# Remote Audio Streaming Validation Plan

## Overview

This document describes the testing and validation procedures for the remote CW audio streaming feature implemented for the IU3QEZ/QRS2HST keyer firmware.

## Feature Summary

**Capability**: Receive and play remote CW audio from DL4YHF's CWNet server during RX mode (when NOT transmitting)

**Audio Format**:
- Codec: A-Law (ITU-T G.711)
- Sample rate: 8 kHz (network) → 16 kHz (local codec)
- Upsampling: 2x linear interpolation
- Ring buffer: 1280 samples @ 8kHz = 160ms capacity
- Min buffering: 320 samples (40ms) before playback start

**Mode Switching**:
- TX mode: Local tone generator (sidetone)
- RX mode: Remote audio stream playback
- Switching: Automatic based on paddle state + 200ms PTT timeout

## Testing Phases

### Phase 1: Unit Testing (Host Tests)

**Location**: `tests_host/audio_stream_player_test.cpp`

**Execute**:
```bash
cd tests_host
mkdir -p build && cd build
cmake ..
make
./all_host_tests --gtest_filter=AudioStreamPlayer*
```

**Expected Results**:
- ✅ All 7 AudioStreamPlayer tests pass
- ✅ Initial state: empty buffer, not playing
- ✅ Write A-Law samples: buffer accepts data
- ✅ Playback start: begins after 320 samples (40ms)
- ✅ Read before ready: returns silence
- ✅ Upsampling: 8kHz → 16kHz interpolation correct
- ✅ Underrun detection: stops playback, increments counter

**Pass Criteria**: Zero test failures

---

### Phase 2: Firmware Build Verification

**Execute**:
```bash
idf.py build
```

**Expected Results**:
- ✅ Build completes without errors
- ✅ No warnings in audio_stream_player.cpp compilation
- ✅ No warnings in sidetone_service.cpp (dual-mode)
- ✅ No warnings in remote_cw_client.cpp (CMD_AUDIO handler)
- ✅ Binary size increase: ~8-10 KB (A-Law tables + ring buffer logic)

**Pass Criteria**: Clean build with no errors

---

### Phase 3: Hardware Integration Testing

#### Prerequisites

**Hardware**:
- IU3QEZ/QRS2HST keyer with ES8311 codec
- WiFi network access
- DL4YHF CWNet server (or compatible) sending audio

**Firmware Configuration**:
```
remote.enabled=true
remote.server_host=<server_ip>
remote.server_port=7373
remote.auto_reconnect=true
remote.ptt_tail_ms=200
```

#### Test 3.1: Connection and Audio Reception

**Steps**:
1. Flash firmware to device
2. Configure remote server settings via Web UI
3. Start remote connection
4. Monitor serial output for connection events

**Expected Serial Output**:
```
I (xxx) init_phases: AudioStreamPlayer injected into RemoteCwClient for remote audio streaming
I (xxx) RemoteCwClient: Connected to server
I (xxx) RemoteCwClient: Received CMD_AUDIO frame (payload_size=64)
I (xxx) SidetoneService: Switching audio mode: ToneGenerator → StreamPlayer
```

**Pass Criteria**:
- ✅ RemoteCwClient connects to server
- ✅ CMD_AUDIO frames logged (payload size ~64-256 bytes)
- ✅ No "buffer full" warnings during steady streaming
- ✅ No crashes or watchdog resets

#### Test 3.2: TX/RX Mode Switching

**Steps**:
1. Ensure remote connection established
2. Server transmitting audio (remote station sending CW)
3. Verify audio output from speaker (RX mode)
4. Press paddle (local transmission)
5. Verify sidetone output (TX mode)
6. Release paddle, wait 200ms
7. Verify audio switches back to remote stream (RX mode)

**Expected Behavior**:

| Action | Audio Mode | Audio Source | LED Activity |
|--------|-----------|--------------|--------------|
| Idle (remote TX) | RX | Remote stream | Blue (WiFi connected) |
| Press paddle | TX | Local tone generator | Cyan (keying active) |
| Release paddle | TX (PTT tail) | Local tone generator | Cyan (keying active) |
| After 200ms | RX | Remote stream | Blue (WiFi connected) |

**Pass Criteria**:
- ✅ Immediate switch to TX mode on paddle press
- ✅ Sidetone heard during TX
- ✅ Remote audio resumes after PTT timeout
- ✅ No audio glitches or clicks during transitions

#### Test 3.3: Buffer Underrun Recovery

**Steps**:
1. Establish remote connection with audio streaming
2. Introduce network latency spike (e.g., WiFi interference)
3. Monitor serial output for underrun events
4. Verify audio recovers after network stabilizes

**Expected Serial Output**:
```
W (xxx) AudioStreamPlayer: Underrun detected (available=240, requested=256)
I (xxx) AudioStreamPlayer: Playback stopped due to underrun
I (xxx) AudioStreamPlayer: Buffer refilled to 320 samples, restarting playback
```

**Pass Criteria**:
- ✅ Underrun detected and logged
- ✅ Playback stops (silence instead of garbage audio)
- ✅ Playback restarts automatically when buffer refills
- ✅ No crashes or permanent audio loss

#### Test 3.4: Long-Duration Stability

**Steps**:
1. Start remote connection with continuous audio streaming
2. Leave running for 1 hour minimum
3. Monitor for crashes, memory leaks, watchdog resets

**Expected Behavior**:
- ✅ No crashes or resets
- ✅ No gradual audio degradation
- ✅ Buffer statistics remain stable (no drift)
- ✅ Heap usage stable (no memory leaks)

**Pass Criteria**: Zero errors over 1-hour test period

---

### Phase 4: Edge Case Testing

#### Test 4.1: Empty Server (No Audio)

**Setup**: Connect to server that's not sending CMD_AUDIO frames

**Expected Behavior**:
- ✅ RX mode active but silent (no audio)
- ✅ No errors logged
- ✅ TX mode still works normally

#### Test 4.2: Rapid TX/RX Switching

**Setup**: Rapidly press/release paddle (5-10 times per second)

**Expected Behavior**:
- ✅ Audio mode switches correctly each time
- ✅ No audio artifacts or buffer corruption
- ✅ No crashes or assertion failures

#### Test 4.3: Disconnection/Reconnection

**Steps**:
1. Establish connection with audio streaming
2. Kill server or disconnect network
3. Wait for reconnection
4. Verify audio resumes

**Expected Behavior**:
- ✅ Graceful disconnection handling
- ✅ Automatic reconnection (if auto_reconnect=true)
- ✅ Audio stream resumes after reconnect
- ✅ No buffer state corruption

---

## Debugging Tools

### Serial Console Commands

```bash
# Check remote connection status
remote status

# Check audio subsystem state
audio status

# Check buffer statistics
decoder stats  # (if morse decoder enabled)
```

### ESP-IDF Logging

Enable verbose logging for debugging:
```c
// In components/audio_subsystem/sidetone_service.cpp
esp_log_level_set("SidetoneService", ESP_LOG_VERBOSE);

// In components/remote/remote_cw_client.cpp
esp_log_level_set("RemoteCwClient", ESP_LOG_VERBOSE);
```

### Logic Analyzer Capture

Monitor audio quality with logic analyzer:
- **I2S DOUT**: Pin from i2s_dout configuration
- **I2S BCLK**: Bit clock
- **I2S LRCK**: Word clock (frame sync)
- **Expected**: Clean transitions between TX/RX modes, no glitches

---

## Known Limitations

1. **Latency**: Total end-to-end latency ~100-150ms (network + buffering)
   - This is acceptable for monitoring but not for real-time QSO

2. **Audio Quality**: A-Law codec has ~13-bit dynamic range
   - Sufficient for CW tones but not hi-fi audio

3. **Network Dependency**: Requires stable WiFi connection
   - Packet loss causes underruns and temporary silence

4. **Single Stream**: Only one audio source at a time (TX or RX)
   - No simultaneous local sidetone + remote audio mixing

---

## Success Criteria Summary

**MUST PASS** (Critical):
- ✅ All unit tests pass
- ✅ Firmware builds without errors
- ✅ Remote audio playback works in RX mode
- ✅ TX mode sidetone works as before
- ✅ No crashes or watchdog resets during normal operation

**SHOULD PASS** (Important):
- ✅ Automatic TX/RX switching based on paddle state
- ✅ Clean audio transitions (no clicks/pops)
- ✅ Buffer underrun recovery without manual intervention
- ✅ 1-hour stability test passes

**NICE TO HAVE** (Optional):
- ✅ Low latency (<100ms network + 40ms buffer)
- ✅ Graceful handling of network interruptions
- ✅ Minimal CPU usage increase (<5%)

---

## Revision History

| Date | Author | Change |
|------|--------|--------|
| 2025-11-14 | Claude | Initial version (Task 8) |
