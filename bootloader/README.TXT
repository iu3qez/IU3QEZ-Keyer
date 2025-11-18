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


IMPORTANT NOTES:
---------------
- Do NOT unplug device during update
- Your configuration will be preserved (NVS unchanged)
- Only .uf2 files are accepted
- Invalid files will be rejected


FACTORY RESET:
-------------
To reset device to factory defaults:
  1. Delete FACTORY_RESET.TXT from this drive
  2. Device will erase NVS and restart with defaults
  3. WARNING: This erases all configuration!


TROUBLESHOOTING:
---------------
If device is stuck in bootloop:
  - Automatic recovery after 3 failed boots
  - OR manually reset via FACTORY_RESET.TXT (above)


MORE INFO:
---------
Project: https://github.com/iu3qez/keyer_qrs2hst
Docs:    docs/FIRMWARE_UPDATE.md
Support: Open issue on GitHub
