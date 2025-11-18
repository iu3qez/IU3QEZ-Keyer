#pragma once

/**
 * @file version.hpp
 * @brief Firmware version information
 *
 * This file defines the firmware version string displayed in the console
 * welcome banner and other user-facing interfaces.
 *
 * VERSION FORMAT: MAJOR.MINOR.PATCH[-SUFFIX]
 * - MAJOR: Incremented for breaking changes or major feature releases
 * - MINOR: Incremented for new features with backward compatibility
 * - PATCH: Incremented for bug fixes
 * - SUFFIX: Optional pre-release identifier (e.g., -alpha, -beta, -rc1)
 *
 * FUTURE ENHANCEMENTS:
 * - Generate version from git describe (git tag + commit hash + dirty flag)
 * - Add build timestamp
 * - Add ESP-IDF version information
 */

namespace app {

/**
 * @brief Firmware version string (semantic versioning format)
 *
 * Update this string for each release following semantic versioning:
 * - 1.0.0: Initial release with USB console, iambic keying, sidetone
 * - 1.1.0: Add web UI configuration interface
 * - 1.2.0: Add RemoteCW network protocol
 * - 2.0.0: Major architecture changes
 */
constexpr const char* kFirmwareVersion = "1.0.0-alpha";

/**
 * @brief Project name
 */
constexpr const char* kProjectName = "Keyer QRS2HST";

/**
 * @brief Full version string for display
 *
 * Example: "Keyer QRS2HST Firmware v1.0.0-alpha"
 */
constexpr const char* kFullVersionString = "Keyer QRS2HST Firmware v1.0.0-alpha";

}  // namespace app
