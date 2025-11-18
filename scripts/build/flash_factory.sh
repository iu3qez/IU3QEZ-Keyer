#!/usr/bin/env bash
#
# Flash tinyuf2 bootloader to factory partition
# Usage: ./flash_factory.sh [port] [bootloader.bin]
#
# Flashes the compiled tinyuf2 bootloader to the factory app partition
# allowing USB-based firmware updates via drag-and-drop UF2 files.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Default values
DEFAULT_PORT="/dev/ttyUSB0"
DEFAULT_BOOTLOADER="${PROJECT_ROOT}/bootloader/tinyuf2/build-keyer_qrs2hst/bootloader-keyer_qrs2hst.bin"
FACTORY_OFFSET="0x34000"  # From partitions.csv

# Parse arguments
PORT="${1:-${DEFAULT_PORT}}"
BOOTLOADER_BIN="${2:-${DEFAULT_BOOTLOADER}}"

if [ ! -f "${BOOTLOADER_BIN}" ]; then
    echo "Error: Bootloader binary not found: ${BOOTLOADER_BIN}"
    echo ""
    echo "Build tinyuf2 bootloader first:"
    echo "  cd ${PROJECT_ROOT}/bootloader/tinyuf2/ports/espressif"
    echo "  make BOARD=keyer_qrs2hst all"
    echo ""
    echo "Usage: $0 [port] [bootloader.bin]"
    echo "  port:           Serial port (default: ${DEFAULT_PORT})"
    echo "  bootloader.bin: Path to bootloader binary (default: ${DEFAULT_BOOTLOADER})"
    exit 1
fi

echo "╔══════════════════════════════════════════════════════════╗"
echo "║       FLASH TINYUF2 BOOTLOADER TO FACTORY PARTITION      ║"
echo "╠══════════════════════════════════════════════════════════╣"
echo "║ Port:         ${PORT}"
echo "║ Bootloader:   ${BOOTLOADER_BIN}"
echo "║ Offset:       ${FACTORY_OFFSET} (factory partition)"
echo "║ Target:       ESP32-S3 Keyer QRS2HST"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Check if esptool.py is available
if ! command -v esptool.py &> /dev/null; then
    echo "Error: esptool.py not found"
    echo "Install: pip install esptool"
    exit 1
fi

# Flash bootloader to factory partition
echo "Flashing bootloader..."
esptool.py --chip esp32s3 \
    --port "${PORT}" \
    --baud 921600 \
    write_flash \
    "${FACTORY_OFFSET}" \
    "${BOOTLOADER_BIN}"

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Bootloader flashed successfully!"
    echo ""
    echo "Next steps:"
    echo "  1. Build main firmware: idf.py build"
    echo "  2. Flash main firmware: idf.py flash"
    echo "  3. Test bootloader entry: console 'upgrade' command"
    echo "  4. Verify USB drive 'KEYERBOOT' appears"
else
    echo ""
    echo "✗ Flash failed"
    exit 1
fi
