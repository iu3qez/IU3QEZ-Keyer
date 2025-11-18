#include "config/keying_presets.hpp"

#include <array>
#include <cassert>
#include <cctype>
#include <cstring>

namespace config {

namespace {

/**
 * @brief Preset configuration lookup table - single source of truth for all preset behaviors
 *
 * Indexed by KeyingPreset enum value (0-9 for V0-V9).
 * Each entry defines complete timing and memory behavior for that preset family.
 *
 * STRUCTURE: {memory_open%, memory_close%, dit_mem, dah_mem, state_latch, L, S, P}
 *
 * PRESET DESIGN RATIONALE:
 *
 * V0-V2 (Super Keyer II/III):
 *   - Tighter memory window (55-99%) for more precise squeeze timing
 *   - State-latch trigger mechanism
 *   - L-S-P timing: 28-48-52 (slightly aggressive, 2.8:1 dash ratio)
 *   - Rationale: SuperKeyer historically uses tighter timing for precision work
 *   - Note: This is an approximation - exact Super Keyer timing TBD
 *
 * V3-V5 (Accukeyer):
 *   - Standard memory window (60-99%)
 *   - State-latch trigger (paddle must remain pressed during window)
 *   - L-S-P timing: 30-50-50 (standard ITU, 3:1 dash ratio)
 *   - Rationale: Accukeyer uses standard PARIS timing (50 dits per word)
 *   - Most forgiving for noisy paddles
 *
 * V6-V8 (Curtis Mode A):
 *   - Standard memory window (60-99%)
 *   - Edge-trigger mechanism (press event captures memory instantly)
 *   - L-S-P timing: 32-52-48 (slightly conservative, 3.2:1 dash ratio)
 *   - Rationale: Curtis Mode A emphasizes reliability with slightly longer elements
 *   - Preferred by operators wanting immediate edge response
 *
 * V9 (No Memory):
 *   - No iambic alternation
 *   - L-S-P timing: 30-50-50 (standard ITU)
 *   - Useful for debugging and straight-key operators
 */
constexpr std::array<PresetConfigData, 10> kPresetConfigurations = {{
    // V0: Super Keyer II/III - Both memories
    {55.0f, 99.0f, true, true, true, 28, 48, 52},
    // V1: Super Keyer II/III - Dit memory only
    {55.0f, 99.0f, true, false, true, 28, 48, 52},
    // V2: Super Keyer II/III - Dah memory only
    {55.0f, 99.0f, false, true, true, 28, 48, 52},
    // V3: Accukeyer - Both memories (DEFAULT PRESET)
    {60.0f, 99.0f, true, true, true, 30, 50, 50},
    // V4: Accukeyer - Dit memory only
    {60.0f, 99.0f, true, false, true, 30, 50, 50},
    // V5: Accukeyer - Dah memory only
    {60.0f, 99.0f, false, true, true, 30, 50, 50},
    // V6: Curtis "A" - Both memories, edge trigger
    {60.0f, 99.0f, true, true, false, 32, 52, 48},
    // V7: Curtis "A" - Dit memory only, edge trigger
    {60.0f, 99.0f, true, false, false, 32, 52, 48},
    // V8: Curtis "A" - Dah memory only, edge trigger
    {60.0f, 99.0f, false, true, false, 32, 52, 48},
    // V9: No memory - Straight key emulation
    {60.0f, 99.0f, false, false, false, 30, 50, 50},
}};

/**
 * @brief Check if a preset configuration appears to be initialized
 *
 * Heuristic: A preset is considered "uninitialized" if all numeric fields are zero.
 * This handles the case where preset_definitions[] array is zero-initialized on first boot.
 *
 * @param preset Preset configuration to check
 * @return true if preset appears valid, false if appears uninitialized
 */
bool IsPresetValid(const PresetConfigData& preset) {
  // Check if all key fields are non-zero (initialized presets always have non-zero values)
  // Memory window percents should be non-zero for valid presets
  if (preset.memory_open_percent == 0.0f && preset.memory_close_percent == 0.0f) {
    return false;  // Likely uninitialized (zero-init from struct default)
  }

  // L-S-P timing should be non-zero for valid presets (defaults: 30-50-50)
  if (preset.timing_l == 0 && preset.timing_s == 0 && preset.timing_p == 0) {
    return false;  // Likely uninitialized
  }

  return true;  // Appears valid
}

}  // namespace

const PresetConfigData& GetPresetConfig(KeyingPreset preset, const DeviceConfig* config) {
  const auto index = static_cast<size_t>(preset);

  // Manual preset has no predefined configuration - caller should check before calling
  assert(preset != KeyingPreset::kManual && "GetPresetConfig called with kManual preset");

  // Validate index is in valid range (0-9) - caller should validate before calling
  assert(index < kPresetConfigurations.size() && "GetPresetConfig called with invalid preset index");

  // If assertions disabled and invalid index, return safe default (Accukeyer Both)
  if (preset == KeyingPreset::kManual || index >= kPresetConfigurations.size()) {
    return kPresetConfigurations[3];  // V3: Accukeyer Both (safe default)
  }

  // Task 3.2: User customization fallback logic
  // 1. If config provided, check user customization first
  if (config != nullptr) {
    const PresetConfigData& user_preset = config->keying.preset_definitions[index];
    if (IsPresetValid(user_preset)) {
      return user_preset;  // Return user-customized preset
    }
    // Fall through to factory default if user preset is invalid/uninitialized
  }

  // 2. Return factory default
  return kPresetConfigurations[index];
}

const PresetConfigData* GetActivePresetConfig(const DeviceConfig& config) {
  // Task 3.4: Return active preset config based on config.keying.preset
  if (config.keying.preset == KeyingPreset::kManual) {
    return nullptr;  // Manual mode - caller uses config.keying.timing_l/s/p directly
  }

  const auto index = static_cast<size_t>(config.keying.preset);
  if (index >= 10) {
    return nullptr;  // Invalid preset index
  }

  // Return reference to preset (either user customization or factory default)
  // Note: Returning pointer to static data or struct member (both have static lifetime)
  return &GetPresetConfig(config.keying.preset, &config);
}

void ApplyPresetDefaults(KeyingConfig& keying) {
  // Manual preset - user has full control, no defaults to apply
  if (keying.preset == KeyingPreset::kManual) {
    return;
  }

  const auto index = static_cast<size_t>(keying.preset);

  // Validate preset is in valid range
  if (index >= kPresetConfigurations.size()) {
    return;  // Invalid preset - leave config unchanged
  }

  // Apply preset configuration
  const PresetConfigData& preset = kPresetConfigurations[index];
  keying.memory_open_percent = preset.memory_open_percent;
  keying.memory_close_percent = preset.memory_close_percent;
  keying.manual_memory_enable_dit = preset.enable_dit_memory;
  keying.manual_memory_enable_dah = preset.enable_dah_memory;
  keying.manual_use_state_latch = preset.use_state_latch;
}

const char* GetPresetName(KeyingPreset preset) {
  switch (preset) {
    case KeyingPreset::kSuperKeyerBoth:
      return "V0";
    case KeyingPreset::kSuperKeyerDot:
      return "V1";
    case KeyingPreset::kSuperKeyerDash:
      return "V2";
    case KeyingPreset::kAccukeyerBoth:
      return "V3";
    case KeyingPreset::kAccukeyerDot:
      return "V4";
    case KeyingPreset::kAccukeyerDash:
      return "V5";
    case KeyingPreset::kCurtisABoth:
      return "V6";
    case KeyingPreset::kCurtisADot:
      return "V7";
    case KeyingPreset::kCurtisADash:
      return "V8";
    case KeyingPreset::kNoMemory:
      return "V9";
    case KeyingPreset::kManual:
    default:
      return "MANUAL";
  }
}

const char* GetPresetDescription(KeyingPreset preset) {
  switch (preset) {
    case KeyingPreset::kSuperKeyerBoth:
      return "SuperKeyer Both";
    case KeyingPreset::kSuperKeyerDot:
      return "SuperKeyer Dit";
    case KeyingPreset::kSuperKeyerDash:
      return "SuperKeyer Dah";
    case KeyingPreset::kAccukeyerBoth:
      return "Accukeyer Both";
    case KeyingPreset::kAccukeyerDot:
      return "Accukeyer Dit";
    case KeyingPreset::kAccukeyerDash:
      return "Accukeyer Dah";
    case KeyingPreset::kCurtisABoth:
      return "Curtis A Both";
    case KeyingPreset::kCurtisADot:
      return "Curtis A Dit";
    case KeyingPreset::kCurtisADash:
      return "Curtis A Dah";
    case KeyingPreset::kNoMemory:
      return "No Memory";
    case KeyingPreset::kManual:
    default:
      return "Manual";
  }
}

bool ParsePresetName(const char* name, KeyingPreset* out_preset) {
  if (name == nullptr || out_preset == nullptr) {
    return false;
  }

  // Convert to lowercase for case-insensitive comparison
  char lower[16] = {};
  size_t i = 0;
  while (name[i] != '\0' && i < sizeof(lower) - 1) {
    lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    ++i;
  }
  lower[i] = '\0';

  // Match preset names
  if (std::strcmp(lower, "v0") == 0) {
    *out_preset = KeyingPreset::kSuperKeyerBoth;
    return true;
  }
  if (std::strcmp(lower, "v1") == 0) {
    *out_preset = KeyingPreset::kSuperKeyerDot;
    return true;
  }
  if (std::strcmp(lower, "v2") == 0) {
    *out_preset = KeyingPreset::kSuperKeyerDash;
    return true;
  }
  if (std::strcmp(lower, "v3") == 0) {
    *out_preset = KeyingPreset::kAccukeyerBoth;
    return true;
  }
  if (std::strcmp(lower, "v4") == 0) {
    *out_preset = KeyingPreset::kAccukeyerDot;
    return true;
  }
  if (std::strcmp(lower, "v5") == 0) {
    *out_preset = KeyingPreset::kAccukeyerDash;
    return true;
  }
  if (std::strcmp(lower, "v6") == 0) {
    *out_preset = KeyingPreset::kCurtisABoth;
    return true;
  }
  if (std::strcmp(lower, "v7") == 0) {
    *out_preset = KeyingPreset::kCurtisADot;
    return true;
  }
  if (std::strcmp(lower, "v8") == 0) {
    *out_preset = KeyingPreset::kCurtisADash;
    return true;
  }
  if (std::strcmp(lower, "v9") == 0) {
    *out_preset = KeyingPreset::kNoMemory;
    return true;
  }
  if (std::strcmp(lower, "manual") == 0) {
    *out_preset = KeyingPreset::kManual;
    return true;
  }

  return false;  // No match
}

}  // namespace config
