// Bootloader Entry Logic for tinyuf2 UF2 Bootloader
// Copyright (c) 2025 IU3QEZ
//
// Provides functions to enter UF2 bootloader mode via RTC memory magic value.
// The bootloader checks this magic value on boot and enters USB mass storage mode
// if set, allowing drag-and-drop firmware updates.

#ifndef APP_BOOTLOADER_ENTRY_HPP_
#define APP_BOOTLOADER_ENTRY_HPP_

#include <cstdint>
#include "esp_err.h"

namespace app {

// Magic value to signal bootloader entry (0xB007ACED = "BOOT ACED")
// Written to RTC memory which survives soft reset but not power loss
constexpr uint32_t kBootloaderMagic = 0xB007ACED;

// Enter UF2 bootloader mode
// Writes magic value to RTC memory and triggers soft reset.
// After reset, bootloader checks magic and enters USB mass storage mode.
//
// Usage:
//   EnterBootloaderMode();  // Device will restart into bootloader
//
void EnterBootloaderMode();

// Check if bootloader flag is set
// Called early in boot sequence (before NVS init) to detect bootloader entry request.
// If magic value is present, clears it and returns true.
//
// Returns:
//   true if bootloader flag was set (device should jump to factory partition)
//   false if normal boot should proceed
//
bool CheckBootloaderFlag();

// Clear bootloader flag
// Used internally by CheckBootloaderFlag() and for testing/debugging.
// Ensures single-use of magic value (not sticky across multiple boots).
//
void ClearBootloaderFlag();

}  // namespace app

#endif  // APP_BOOTLOADER_ENTRY_HPP_
