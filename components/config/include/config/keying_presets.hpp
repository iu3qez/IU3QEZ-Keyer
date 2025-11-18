#pragma once

/**
 * @file keying_presets.hpp
 * @brief Centralized keying preset definitions and configuration utilities
 *
 * This module provides the single source of truth for all iambic keyer preset
 * configurations (V0-V9). Each preset defines complete timing parameters,
 * memory behavior, and trigger mechanisms for different keyer families.
 *
 * ARCHITECTURE RATIONALE:
 * - Separates preset configuration data from UI/command handling
 * - Ensures preset definitions are reusable across console, web UI, and storage
 * - Eliminates code duplication in preset application logic
 * - Makes preset behavior testable in isolation
 *
 * PRESET FAMILIES:
 * - V0-V2: Super Keyer II/III timing (state-latch, 55% window)
 * - V3-V5: Accukeyer timing (state-latch, 60% window)
 * - V6-V8: Curtis Mode A timing (edge-trigger, 60% window)
 * - V9: No memory (straight key emulation)
 *
 * See IAMBIC_MODES.md for detailed timing diagrams and behavioral specifications.
 */

#include "config/device_config.hpp"

namespace config {

// NOTE: PresetConfigData struct is now defined in device_config.hpp
// (moved to avoid circular dependency - device_config.hpp needs PresetConfigData
// for KeyingConfig.preset_definitions[] array)

/**
 * @brief Get preset configuration data for a given preset enum
 *
 * Returns preset configuration from user customization (if available) or factory defaults.
 * This is the authoritative source for preset behavior at runtime.
 *
 * @param preset The preset to query (V0-V9, NOT kManual)
 * @param config Optional device configuration containing user customizations
 *               (nullptr = use factory defaults only)
 * @return Const reference to preset configuration data
 *
 * @note Task 3.2: Fallback logic
 *       1. If config != nullptr: Check config.keying.preset_definitions[index]
 *       2. If user preset is valid (non-zero): Return user preset
 *       3. Otherwise: Return factory default from kPresetConfigurations[]
 *
 * @note Caller must validate preset is in range [0-9] and NOT kManual before calling.
 *       Debug builds will assert on invalid input. Release builds return Accukeyer (V3)
 *       as safe default for invalid presets.
 *
 * Example:
 * ```
 * DeviceConfig cfg = storage.LoadOrDefault();
 * const auto& preset = GetPresetConfig(KeyingPreset::kAccukeyerBoth, &cfg);
 * // Returns user-customized V3 if available, else factory default
 * ```
 */
const PresetConfigData& GetPresetConfig(KeyingPreset preset, const DeviceConfig* config = nullptr);

/**
 * @brief Get active preset configuration based on current preset selection
 *
 * Convenience helper that returns preset config for config.keying.preset.
 * Skips kManual (returns nullptr for manual mode).
 *
 * @param config Device configuration containing preset selection and customizations
 * @return Pointer to active preset config, or nullptr if preset == kManual
 *
 * @note Task 3.4: Helper for simplified runtime access
 *       - Automatically selects correct preset based on config.keying.preset
 *       - Returns nullptr for kManual (caller should check before dereferencing)
 *       - Returns user customization if available, else factory default
 *
 * Example:
 * ```
 * DeviceConfig cfg = storage.LoadOrDefault();
 * const PresetConfigData* active = GetActivePresetConfig(cfg);
 * if (active != nullptr) {
 *   // Use active->timing_l, active->memory_open_percent, etc.
 * } else {
 *   // Manual mode - use cfg.keying.timing_l/s/p directly
 * }
 * ```
 */
const PresetConfigData* GetActivePresetConfig(const DeviceConfig& config);

/**
 * @brief Apply preset defaults to a keying configuration
 *
 * Modifies the provided KeyingConfig to match the preset's standard parameters.
 * Only applies defaults for non-manual presets (V0-V9).
 * Manual preset is ignored - manual parameters remain untouched.
 *
 * @param keying Keying configuration to modify (preset field must be set)
 *
 * Side effects:
 * - Updates memory_open_percent, memory_close_percent
 * - Updates consider_late_release
 * - Updates manual_memory_enable_dit/dah
 * - Updates manual_use_state_latch
 *
 * Example:
 * ```
 * KeyingConfig cfg;
 * cfg.preset = KeyingPreset::kCurtisABoth;
 * ApplyPresetDefaults(cfg);  // cfg now has Curtis A timing parameters
 * ```
 */
void ApplyPresetDefaults(KeyingConfig& keying);

/**
 * @brief Get human-readable name for a preset
 *
 * @param preset Preset enum value
 * @return String representation (e.g., "V3", "V6", "MANUAL")
 */
const char* GetPresetName(KeyingPreset preset);

/**
 * @brief Get human-readable description for a preset
 *
 * @param preset Preset enum value
 * @return Description string (e.g., "Accukeyer Both", "Curtis A Dit")
 *
 * @note Task 4.2: Used by REST API and console commands
 *       Maps preset enum to full description for UI display
 *
 * Example:
 * ```
 * const char* desc = GetPresetDescription(KeyingPreset::kAccukeyerBoth);
 * // desc == "Accukeyer Both"
 * ```
 */
const char* GetPresetDescription(KeyingPreset preset);

/**
 * @brief Parse preset name string to enum
 *
 * Accepts case-insensitive strings: "V0"-"V9", "manual"
 *
 * @param name Preset name string
 * @param out_preset Output parameter for parsed preset (only written on success)
 * @return true if parse succeeded, false otherwise
 *
 * Example:
 * ```
 * KeyingPreset preset;
 * if (ParsePresetName("v3", &preset)) {
 *   // preset == KeyingPreset::kAccukeyerBoth
 * }
 * ```
 */
bool ParsePresetName(const char* name, KeyingPreset* out_preset);

}  // namespace config
