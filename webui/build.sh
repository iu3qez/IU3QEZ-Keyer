#!/usr/bin/env bash
# Build script for WebUI that runs in a clean environment
set -e

cd "$(dirname "$0")"

# Unset ESP-IDF specific variables that interfere with npm build
unset IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_PATH IDF_TARGET
unset OPENOCD_SCRIPTS ESPPORT ESPBAUD ESP_IDF_VERSION
unset IDF_COMPONENT_MANAGER IDF_MAINTAINER

# Prepend node_modules/.bin to PATH so local tools are found first
export PATH="$(pwd)/node_modules/.bin:$PATH"

# Run npm build
npm run build

echo "WebUI build completed successfully"
