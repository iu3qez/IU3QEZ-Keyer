#!/usr/bin/env bash
#
# Generate UF2 firmware file from ESP32-S3 binary
# Usage: ./generate_uf2.sh <input.bin> <output.uf2> [base_address]
#
# Converts ESP-IDF binary firmware to UF2 format for tinyuf2 bootloader
# Family ID: 0xc47e5767 (ESP32-S3)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UF2CONV="${SCRIPT_DIR}/uf2conv.py"

# ESP32-S3 Family ID from Microsoft UF2 families
FAMILY_ID="0xc47e5767"

# Default base address for ESP32-S3 application (after bootloader + partition table)
# OTA partition starts at 0x134000 according to partitions.csv (with factory bootloader)
DEFAULT_BASE_ADDR="0x134000"

# Parse arguments
INPUT_BIN="${1}"
OUTPUT_UF2="${2}"
BASE_ADDR="${3:-${DEFAULT_BASE_ADDR}}"

if [ -z "${INPUT_BIN}" ] || [ -z "${OUTPUT_UF2}" ]; then
    echo "Usage: $0 <input.bin> <output.uf2> [base_address]"
    echo ""
    echo "Example:"
    echo "  $0 build/keyer_qrs2hst.bin build/firmware.uf2"
    echo "  $0 build/keyer_qrs2hst.bin build/firmware.uf2 0x134000"
    echo ""
    echo "Default base address: ${DEFAULT_BASE_ADDR} (ota_0 partition offset)"
    exit 1
fi

if [ ! -f "${INPUT_BIN}" ]; then
    echo "Error: Input file not found: ${INPUT_BIN}"
    exit 1
fi

if [ ! -f "${UF2CONV}" ]; then
    echo "Error: uf2conv.py not found at ${UF2CONV}"
    echo "Run: curl -o ${UF2CONV} https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2conv.py"
    exit 1
fi

# Convert binary to UF2
echo "Converting ${INPUT_BIN} to UF2 format..."
echo "  Base address: ${BASE_ADDR}"
echo "  Family ID:    ${FAMILY_ID} (ESP32-S3)"
echo "  Output:       ${OUTPUT_UF2}"

python3 "${UF2CONV}" \
    --convert \
    --base "${BASE_ADDR}" \
    --family "${FAMILY_ID}" \
    --output "${OUTPUT_UF2}" \
    "${INPUT_BIN}"

if [ $? -eq 0 ]; then
    FILE_SIZE=$(stat -c%s "${OUTPUT_UF2}" 2>/dev/null || stat -f%z "${OUTPUT_UF2}" 2>/dev/null)
    echo "✓ UF2 file generated successfully"
    echo "  Size: ${FILE_SIZE} bytes"
    echo "  Ready to flash via tinyuf2 bootloader"
else
    echo "✗ UF2 conversion failed"
    exit 1
fi
