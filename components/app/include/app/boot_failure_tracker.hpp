#pragma once

/**
 * @file boot_failure_tracker.hpp
 * @brief Boot failure tracking and bootloop detection
 *
 * Tracks consecutive boot failures using RTC memory to detect bootloop scenarios
 * and trigger safe mode for recovery. Uses the same RTC persistence mechanism
 * as bootloader_entry.hpp.
 *
 * ARCHITECTURE RATIONALE:
 * - RTC memory persists across soft resets but not power cycles
 * - Consecutive boot failures indicate NVS corruption or fatal init errors
 * - Safe mode allows user to trigger factory reset via bootloader
 * - Threshold of 3 failures balances false positives vs safety
 *
 * BOOT FAILURE LIFECYCLE:
 * 1. Boot starts → IncrementBootFailureCount() increments counter
 * 2. Init proceeds through all phases
 * 3. If successful → ClearBootFailureCount() resets counter to 0
 * 4. If failure occurs → device restarts, counter persists
 * 5. After 3 consecutive failures → safe mode enters bootloader
 *
 * THREAD SAFETY:
 * - All functions must be called from main task during init
 * - RTC memory access is NOT interrupt-safe
 * - Do not call from ISRs or multiple tasks
 *
 * USAGE:
 * 1. Call IncrementBootFailureCount() early in init (Phase 1)
 * 2. Call IsBootloopDetected() to check threshold (Phase 2)
 * 3. If detected, enter safe mode (bootloader with LED indication)
 * 4. Call ClearBootFailureCount() after successful init (final phase)
 */

#include <cstdint>

namespace app {

/**
 * @brief Boot failure detection threshold
 *
 * Device enters safe mode after this many consecutive boot failures.
 * Threshold of 3 balances:
 * - False positives: Unlikely to have 3 accidental failures
 * - User safety: Quick recovery without excessive retries
 * - Power cycle: User can power cycle to clear counter if needed
 */
constexpr uint8_t kBootFailureThreshold = 3;

/**
 * @brief Increment boot failure counter
 *
 * Call at the start of boot sequence (before NVS init) to track
 * consecutive failures. Counter persists in RTC memory across
 * soft resets but is cleared on power cycle.
 *
 * @return Current failure count after increment
 *
 * Thread safety: Main task only during init
 * Side effects: Modifies RTC memory
 */
uint8_t IncrementBootFailureCount();

/**
 * @brief Check if bootloop condition is detected
 *
 * Returns true if failure count >= threshold, indicating
 * persistent boot failures that require safe mode recovery.
 *
 * @return true if bootloop detected (count >= kBootFailureThreshold)
 *
 * Thread safety: Main task only during init
 * Side effects: None (read-only)
 */
bool IsBootloopDetected();

/**
 * @brief Clear boot failure counter
 *
 * Call after successful boot completion to reset the counter.
 * This prevents entering safe mode on the next boot.
 *
 * Thread safety: Main task only during init
 * Side effects: Clears RTC memory counter
 */
void ClearBootFailureCount();

/**
 * @brief Get current boot failure count
 *
 * Read-only access to failure counter for diagnostics/logging.
 *
 * @return Current failure count
 *
 * Thread safety: Main task only during init
 * Side effects: None (read-only)
 */
uint8_t GetBootFailureCount();

}  // namespace app
