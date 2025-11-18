# NVS Recovery and Bootloop Protection

This guide explains the Keyer QRS2HST's automatic recovery mechanisms and how to recover from bootloop scenarios caused by NVS corruption or firmware issues.

## Overview

The device includes **bootloop protection** that automatically detects repeated boot failures and enters safe mode for recovery. This prevents devices from becoming unrecoverable due to NVS corruption or bad firmware.

### Key Features

- **Automatic detection**: Tracks consecutive boot failures using RTC memory
- **Safe mode entry**: Enters bootloader after 3 failed boots
- **NVS reset trigger**: Delete special file to erase configuration
- **Manual reset option**: Console command for intentional resets
- **Configuration backup**: Future feature (export/import)

---

## Boot Failure Tracking

### How It Works

1. **On every boot**: Failure counter increments (stored in RTC memory)
2. **During init**: Each phase executes sequentially
3. **If boot fails**: Device restarts, counter persists (soft reset)
4. **If boot succeeds**: Counter clears to 0 automatically
5. **If 3 consecutive failures**: Safe mode activated

**RTC Memory Characteristics:**
- Persists across soft resets (`esp_restart()`)
- Cleared on power cycle (unplug device)
- Independent of NVS flash storage

### Boot Failure Threshold

```cpp
constexpr uint8_t kBootFailureThreshold = 3;
```

**Rationale:**
- **1-2 failures**: May be transient (voltage spike, timing)
- **3 failures**: Indicates persistent problem (NVS corruption, bad firmware)
- **Power cycle**: User can clear counter by unplugging device

### What Counts as a Failure

Boot fails if:
- NVS initialization fails (corrupted partition)
- Configuration loading crashes (invalid data)
- Critical subsystem init fails (GPIO, I2C, etc.)
- Device restarts before `ClearBootFailureCount()` called

Boot succeeds if:
- All 18 init phases complete successfully
- Main loop starts running
- Counter cleared automatically

---

## Automatic Recovery (Safe Mode)

### Detection

When bootloop detected (3 consecutive failures):

```
E (1234) boot_failure: ╔══════════════════════════════════════════════════════════╗
E (1235) boot_failure: ║              BOOTLOOP DETECTED                           ║
E (1236) boot_failure: ╠══════════════════════════════════════════════════════════╣
E (1237) boot_failure: ║ Consecutive boot failures: 3 (threshold: 3)             ║
E (1238) boot_failure: ║                                                          ║
E (1239) boot_failure: ║ Device will enter SAFE MODE for recovery                ║
E (1240) boot_failure: ║                                                          ║
E (1241) boot_failure: ║ Recovery options:                                        ║
E (1242) boot_failure: ║  1. Enter bootloader (automatic)                         ║
E (1243) boot_failure: ║  2. Delete FACTORY_RESET.TXT from bootloader drive      ║
E (1244) boot_failure: ║  3. Device will erase NVS and restart with defaults      ║
E (1245) boot_failure: ╚══════════════════════════════════════════════════════════╝
```

### Safe Mode Actions

1. **Device enters bootloader automatically**
   - Same as running `upgrade` command
   - Appears as USB drive "KEYERBOOT"

2. **LED indication** (future enhancement):
   - Rapid red blinking (5Hz) indicates safe mode
   - Distinguishes from normal bootloader (orange pulsing)

3. **Recovery options available**:
   - Factory reset via FACTORY_RESET.TXT trigger
   - Reflash firmware via UF2 file
   - Exit without changes (power cycle clears counter)

### Recovery Process

#### Option 1: Factory Reset (Recommended)

**When to use:**
- NVS corrupted (most common cause)
- Unknown bad configuration
- Want to start fresh with defaults

**Steps:**

1. **Device in bootloader** (automatic after 3 failures)
   ```bash
   # KEYERBOOT drive should appear
   ls /media/<user>/KEYERBOOT/
   ```

2. **Delete trigger file**:
   ```bash
   rm /media/<user>/KEYERBOOT/FACTORY_RESET.TXT
   ```

3. **Device detects deletion and**:
   - Erases entire NVS partition
   - Restarts with factory defaults
   - Boots normally (counter reset)

4. **Reconfigure device**:
   - WiFi credentials lost (reconfigure)
   - Keyer settings reset to defaults
   - All parameters need reconfiguration

#### Option 2: Reflash Firmware

**When to use:**
- Suspect bad firmware caused bootloop
- Want to try different firmware version
- Have known good firmware backup

**Steps:**

1. **Copy firmware to bootloader**:
   ```bash
   cp firmware_good.uf2 /media/<user>/KEYERBOOT/
   ```

2. **Device flashes and restarts**

3. **Check if boot succeeds**:
   - If successful: Counter clears, normal operation
   - If fails again: Bootloop counter increments (max 3 more tries)

#### Option 3: Exit Without Changes

**When to use:**
- Investigating issue
- Want to preserve NVS for debugging
- Have alternative recovery method

**Steps:**

1. **Power cycle device** (unplug USB)
   - RTC memory clears (counter resets to 0)
   - Next boot starts with fresh counter

2. **Device attempts normal boot**
   - If problem transient: May succeed
   - If problem persists: Counter increments again

**Note:** This doesn't fix the underlying issue, just resets the counter.

---

## Manual Factory Reset

### Console Command

**When to use:**
- Intentional reset (not bootloop scenario)
- Selling/transferring device
- Testing default configuration
- Debugging configuration issues

**Steps:**

1. **Connect to console**:
   ```bash
   screen /dev/ttyACM0 115200
   ```

2. **Run command**:
   ```
   keyer> factory-reset
   ```

3. **Review warning**:
   ```
   WARNING: This will erase ALL configuration and reboot!

   This action will:
     - Erase all NVS flash data
     - Reset to factory defaults
     - Reboot the device

   To proceed, type: factory-reset confirm
   ```

4. **Confirm**:
   ```
   keyer> factory-reset confirm

   Erasing NVS flash...
   NVS erased successfully!
   Rebooting to factory defaults...
   ```

5. **Device restarts** with default configuration

### Web UI (Future Feature)

Not yet implemented. Will be added in future release:
- Settings → Advanced → Factory Reset button
- Confirmation dialog with password
- Progress indicator during reset

---

## NVS Partition Details

### What's Stored in NVS

**Configuration data:**
- WiFi credentials (SSID, password, mode)
- Keyer parameters (speed, preset, paddle config)
- Audio settings (sidetone frequency, volume)
- Hardware settings (GPIO assignments)
- Remote CW settings (server/client config)
- Decoder settings (tolerance, enabled state)
- User preferences

**Not stored in NVS:**
- Firmware binary (ota_0/ota_1 partitions)
- Bootloader (factory partition)
- System data (phy_init, otadata)

### Partition Layout

```
NVS Partition:
- Offset: 0x9000
- Size: 96KB (98304 bytes)
- Type: data (NVS)
- Encryption: Not enabled (plaintext)
```

### Corruption Causes

**Common causes:**
1. **Power loss during write**: Most common cause
2. **Flash wear**: After ~10,000 write cycles per sector
3. **Voltage instability**: Brown-out during flash operation
4. **Bad firmware**: Bug in config write code
5. **Hardware issue**: Faulty flash chip

**Symptoms:**
- `nvs_flash_init()` returns `ESP_ERR_NVS_NO_FREE_PAGES`
- `nvs_flash_init()` returns `ESP_ERR_NVS_NEW_VERSION_FOUND`
- Device crashes during config load
- Bootloop after power cycle

---

## Preventive Measures

### Minimize Write Cycles

**Current implementation:**
- Config only written when user saves (Web UI or console)
- No auto-save on parameter changes
- Atomic writes (all or nothing)

**Best practices:**
- Avoid frequent "save" operations
- Batch configuration changes
- Don't save in automated scripts unless necessary

### Power Supply Quality

**Recommendations:**
- Use quality USB power supply (5V ≥500mA)
- Avoid USB hubs (use direct PC port or wall adapter)
- Add capacitors on power rails (hardware design)
- Consider UPS for critical deployments

### Firmware Quality

**Testing before release:**
- Verify NVS write operations
- Test bootloop recovery mechanism
- Validate configuration schema changes
- Check for init phase failures

---

## Debugging Boot Failures

### Console Logs

**Enable verbose logging:**
```bash
# Before NVS corruption occurs
keyer> debug verbose
```

**Look for error patterns:**
```
E (123) nvs: nvs_flash_init_custom partition='nvs' err=0x1105 NO_FREE_PAGES
E (124) config: Failed to initialize NVS: 0x1105
E (125) boot_failure: Boot failure count incremented: 1/3
```

### Boot Counter Logs

**First failure:**
```
W (100) boot_failure: Boot failure count incremented: 1/3
```

**Second failure:**
```
W (100) boot_failure: Boot failure count incremented: 2/3
```

**Third failure (bootloop detected):**
```
W (100) boot_failure: Boot failure count incremented: 3/3
E (101) boot_failure: ╔══════════════════════════════════════╗
E (102) boot_failure: ║      BOOTLOOP DETECTED               ║
E (103) boot_failure: ╠══════════════════════════════════════╣
...
```

**Successful boot:**
```
I (5000) boot_failure: Boot successful - clearing failure count (was: 1)
```

### RTC Memory Inspection

**Not exposed via console** (future enhancement)

**Internal state:**
```cpp
RTC_DATA_ATTR static uint8_t rtc_boot_fail_count = 0;
```

**Developer access:**
```bash
# Via GDB (requires JTAG)
(gdb) x/1b &rtc_boot_fail_count
```

---

## Recovery Flowchart

```
┌─────────────────────┐
│   Device Boots      │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│ Increment Counter   │  (RTC memory: count++)
└──────────┬──────────┘
           │
           ▼
      ┌────────┐
      │Count≥3?│
      └────┬───┘
           │
     ┌─────┴─────┐
     │           │
    NO          YES
     │           │
     ▼           ▼
┌─────────┐  ┌──────────────────┐
│Continue │  │ SAFE MODE        │
│  Boot   │  │ Enter Bootloader │
└────┬────┘  └────────┬─────────┘
     │                │
     ▼                ▼
┌─────────┐     ┌──────────────┐
│Success? │     │ User Action: │
└────┬────┘     │ 1. Reset NVS │
     │          │ 2. Reflash   │
┌────┴─────┐    │ 3. Exit      │
│          │    └──────┬───────┘
YES       NO           │
│          │           ▼
▼          ▼      ┌──────────┐
┌────────┐ ┌────┐ │ Device   │
│ Clear  │ │Keep│ │ Restarts │
│Counter │ │Cntr│ └────┬─────┘
└────────┘ └──┬─┘      │
            │          │
            └──────────┘
```

---

## FAQ

**Q: Can I disable bootloop protection?**
A: Not configurable (safety feature). However, power cycling clears the counter.

**Q: What if I want to keep NVS during recovery?**
A: Don't delete FACTORY_RESET.TXT. Instead, reflash firmware or power cycle.

**Q: How do I backup my configuration?**
A: Currently no export feature. Future enhancement: Web UI config export/import.

**Q: Will factory reset erase the bootloader?**
A: No, only NVS partition is erased. Bootloader in factory partition is untouched.

**Q: Can I manually erase NVS without bootloader?**
A: Yes, via console: `factory-reset confirm` or via esptool: `esptool.py erase_region 0x9000 0x18000`

**Q: What happens if NVS corruption occurs during runtime?**
A: Config writes may fail, but device continues running with in-memory config. Save operation will report error.

**Q: Can I set a different threshold (not 3)?**
A: Requires firmware modification. Edit `kBootFailureThreshold` in `boot_failure_tracker.hpp` and rebuild.

**Q: Does factory reset clear the boot counter?**
A: Yes, indirectly. Device boots successfully after reset, which clears the counter.

---

## Advanced: Partition Recovery

### Using esptool.py

**Erase NVS partition:**
```bash
esptool.py --port /dev/ttyUSB0 erase_region 0x9000 0x18000
```

**Flash known good NVS:**
```bash
# If you have NVS backup
esptool.py --port /dev/ttyUSB0 write_flash 0x9000 nvs_backup.bin
```

### Using parttool.py

```bash
# Erase NVS
parttool.py --port /dev/ttyUSB0 erase_partition --partition-name=nvs

# Read NVS (for backup)
parttool.py --port /dev/ttyUSB0 read_partition --partition-name=nvs \
  --output nvs_backup.bin
```

---

## Support

**If recovery doesn't work:**
1. Check bootloader is installed (see `docs/FIRMWARE_UPDATE.md`)
2. Verify USB cable and port
3. Try manual esptool.py erase (above)
4. Open GitHub issue with logs

**Links:**
- Main documentation: `docs/FIRMWARE_UPDATE.md`
- GitHub issues: https://github.com/iu3qez/keyer_qrs2hst/issues
- ESP-IDF NVS docs: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/nvs_flash.html
