# Firmware Update Guide

This guide explains how to update the Keyer QRS2HST firmware using the UF2 bootloader.

## Overview

The device uses **tinyuf2**, a USB mass storage bootloader that makes firmware updates as easy as copying a file. No special tools or software required - just drag and drop!

### Key Features

- **Drag-and-drop updates**: Device appears as USB drive
- **Configuration preserved**: WiFi credentials and settings are kept
- **Automatic validation**: Invalid firmware files are rejected
- **Checksum verification**: Ensures firmware integrity
- **Bootloop recovery**: Automatic safe mode after 3 failed boots

---

## Method 1: Console Command

### Prerequisites

- Device connected via USB or accessible via WiFi
- Serial terminal or Web UI access

### Steps

#### Via Serial Console (USB or UART)

1. **Connect to console**:
   ```bash
   # Linux/macOS
   screen /dev/ttyACM0 115200

   # Or use any serial terminal at 115200 baud
   ```

2. **Enter bootloader mode**:
   ```
   keyer> upgrade
   ```

3. **Review the instructions displayed**:
   ```
   ╔══════════════════════════════════════════════════════════╗
   ║         ENTERING UF2 BOOTLOADER MODE                     ║
   ╠══════════════════════════════════════════════════════════╣
   ║ Device will restart into USB mass storage mode           ║
   ║                                                          ║
   ║ Steps to update firmware:                                ║
   ║   1. Device will appear as USB drive 'KEYERBOOT'         ║
   ║   2. Drag firmware.uf2 file onto the drive               ║
   ║   3. Device will auto-reset with new firmware            ║
   ║   4. Configuration will be preserved                     ║
   ║   5. Reconnect to console after reboot                   ║
   ║                                                          ║
   ║ WARNING: Do NOT unplug during update!                    ║
   ╚══════════════════════════════════════════════════════════╝
   ```

4. **Device restarts** - connection will drop

5. **Continue to "Update Process" section below**

#### Via Web UI

1. **Open web interface**:
   - Navigate to `http://<device-ip>/` or `http://192.168.4.1/` (AP mode)

2. **Go to Firmware Update page**:
   - Click the **"Firmware Update"** card on the home page
   - Or directly navigate to `http://<device-ip>/firmware`

3. **Click "Enter Bootloader Mode"**:
   - Confirm the dialog prompt
   - Device will restart into bootloader mode

4. **Continue to "Update Process" section below**

---

## Update Process

Once the device enters bootloader mode:

### 1. USB Drive Appears

The device will appear as a USB mass storage device named **KEYERBOOT**.

**What you'll see:**
- **Linux**: `/media/<user>/KEYERBOOT` or `/mnt/KEYERBOOT`
- **macOS**: `/Volumes/KEYERBOOT` (appears on desktop)
- **Windows**: New drive letter (e.g., `E:\`, `F:\`)

### 2. Check README

Open `README.TXT` on the drive for quick instructions:
```
KEYER QRS2HST - Firmware Update Mode
=====================================

To update firmware:
  1. Download the latest firmware.uf2 file
  2. Drag and drop the file onto this drive
  3. Wait for device to restart (LED will blink)
  4. Update complete!

CURRENT INFORMATION:
-------------------
Firmware:   Keyer QRS2HST
Bootloader: tinyuf2 for ESP32-S3
Board ID:   ESP32S3-Keyer-QRS2HST-v1
Flash:      16MB

USB VID/PID: 0x303A:0x8002
```

### 3. Drag Firmware File

**Download firmware:**
- Official releases: GitHub releases page
- Custom builds: `build/firmware.uf2` after building from source

**Copy to drive:**
```bash
# Linux/macOS
cp firmware.uf2 /media/<user>/KEYERBOOT/

# Or just drag the file in your file manager
```

**Windows:** Drag `firmware.uf2` onto the KEYERBOOT drive

### 4. Automatic Flash & Reboot

- File is validated and flashed automatically
- LED will blink during the process
- Device restarts with new firmware (~5-10 seconds)
- USB drive disappears when complete

### 5. Verify Update

**Check version** (console):
```
keyer> version
Keyer QRS2HST v2.0.0
Build: 2025-11-06 14:32:00
```

**Check status** (Web UI):
- Home page shows firmware version
- Configuration should be preserved

---

## Building Firmware from Source

### Prerequisites

- ESP-IDF v5.4.3 installed
- Project cloned and configured

### Build Steps

```bash
# Configure project (first time only)
idf.py menuconfig

# Build firmware binary
idf.py build

# Generate UF2 file
ninja -C build firmware-uf2
```

**Output files:**
- `build/iu3qez_qrshst_keyer.bin` - Raw binary (for esptool.py)
- `build/firmware.uf2` - UF2 file (for bootloader)

**UF2 Configuration:**
- **Family ID**: `0xc47e5767` (ESP32-S3)
- **Base Address**: `0x134000` (ota_0 partition with factory bootloader)
- **Partition**: ota_0 (4MB)

### Flash Bootloader (One-Time)

If updating from a version without UF2 bootloader:

```bash
# Build bootloader
cd bootloader/tinyuf2/ports/espressif
make BOARD=keyer_qrs2hst all

# Flash to factory partition
cd ../../../../
./scripts/build/flash_factory.sh /dev/ttyUSB0
```

**Note:** This only needs to be done once. Future updates use the UF2 bootloader.

---

## Troubleshooting

### USB Drive Not Appearing

**Symptoms:**
- Device restarts but no KEYERBOOT drive appears

**Solutions:**
1. **Check USB cable**: Must support data transfer (not charge-only)
2. **Try different port**: Some USB hubs don't work reliably
3. **Check drivers** (Windows): ESP32-S3 USB drivers may be needed
4. **Verify bootloader**: May not be flashed yet (see "Flash Bootloader" above)

**Manual entry (if needed):**
```bash
# Hold BOOT button while powering on (hardware fallback)
# Or use esptool.py to flash bootloader
```

### Update Failed or Device Won't Boot

**Symptoms:**
- Device stuck in bootloop
- Red LED blinking rapidly
- No response from console

**Automatic Recovery:**
Device has bootloop protection:
- After **3 consecutive boot failures**, device enters safe mode
- Automatically enters bootloader mode
- **FACTORY_RESET.TXT** trigger becomes available

**Manual Recovery:**

1. **Enter bootloader** (should happen automatically after 3 failures)

2. **Trigger factory reset**:
   ```bash
   # Delete FACTORY_RESET.TXT from KEYERBOOT drive
   rm /media/<user>/KEYERBOOT/FACTORY_RESET.TXT
   ```

3. **Device erases NVS and restarts** with factory defaults

4. **Re-flash known good firmware** via bootloader

**Alternative: Console Factory Reset**
```
keyer> factory-reset confirm
```

### Firmware Rejected

**Symptoms:**
- File copied but device doesn't restart
- Error message on console

**Causes:**
- Wrong UF2 family ID (not ESP32-S3)
- Corrupted download
- Wrong board configuration

**Solutions:**
1. **Verify UF2 file**:
   ```bash
   # Check family ID (should be 0xc47e5767)
   python3 scripts/build/uf2conv.py -i firmware.uf2
   ```

2. **Re-download** firmware (may be corrupted)

3. **Build from source** if using custom configuration

### Configuration Lost After Update

**Expected behavior:**
- WiFi credentials **preserved**
- Keyer settings **preserved**
- NVS partition **unchanged**

**If configuration lost:**
- May indicate NVS corruption
- Factory reset may have been triggered
- Bootloop recovery activated (check boot counter logs)

**Prevention:**
- Don't flash invalid firmware (bootloop → NVS reset)
- Keep backup of configuration (future feature: export/import)

---

## Advanced Topics

### Downgrading Firmware

**Supported:** Yes, bootloader allows any version

```bash
# No special steps needed - just copy older firmware.uf2
cp firmware_v1.5.0.uf2 /media/<user>/KEYERBOOT/
```

**Note:** Ensure old firmware is compatible with current NVS schema

### OTA Partition Layout

```
Partition Table (16MB flash):
┌─────────────────────────────────────┐
│ nvs (96KB)                          │  0x9000
├─────────────────────────────────────┤
│ otadata (8KB)                       │  0x27000
├─────────────────────────────────────┤
│ phy_init (4KB)                      │  0x29000
├─────────────────────────────────────┤
│ coredump (64KB)                     │  0x2A000
├─────────────────────────────────────┤
│ factory (1MB) - tinyuf2 bootloader  │  0x34000
├─────────────────────────────────────┤
│ ota_0 (4MB) - Current firmware      │  0x134000 ← UF2 flashes here
├─────────────────────────────────────┤
│ ota_1 (4MB) - Backup (unused)       │  0x534000
├─────────────────────────────────────┤
│ spiffs (6896KB) - Future use        │  ...
└─────────────────────────────────────┘
```

### Security Considerations

**Current implementation:**
- ✅ Checksum validation (UF2 built-in)
- ✅ Family ID verification
- ✅ Bootloop protection
- ❌ Firmware signing (not implemented)
- ❌ Secure boot (not enabled)

**For production use**, consider enabling:
- ESP32-S3 secure boot
- Flash encryption
- Signed firmware validation

---

## FAQ

**Q: Will my WiFi settings be erased?**
A: No, configuration is stored in NVS partition which is not touched during firmware updates.

**Q: Can I brick the device?**
A: Very unlikely. Bootloader is in factory partition (separate from firmware). Even if firmware update fails, bootloop protection enters safe mode for recovery.

**Q: How do I know if bootloader is installed?**
A: If `upgrade` command exists and device has factory partition, bootloader is present. Check with: `ls /dev/ttyACM0` after entering bootloader mode.

**Q: Can I update over WiFi?**
A: Not directly. Must enter bootloader mode first (which disables WiFi), then use USB. This is a security feature.

**Q: What if I lose USB access during update?**
A: Device may be stuck in partial flash state. Power cycle and re-enter bootloader to retry update.

**Q: Can I automate updates?**
A: Yes, using serial console commands:
```bash
echo "upgrade" > /dev/ttyACM0
sleep 5
cp firmware.uf2 /media/KEYERBOOT/
```

---

## Support

**Issues or questions?**
- GitHub: https://github.com/iu3qez/keyer_qrs2hst/issues
- Documentation: `docs/` directory
- Console help: `help` command

**Before reporting issues:**
1. Check bootloader is installed (`upgrade` command exists)
2. Verify USB cable supports data transfer
3. Try different USB port
4. Check system logs: `dmesg | grep -i usb` (Linux)
5. Test with known good firmware file
