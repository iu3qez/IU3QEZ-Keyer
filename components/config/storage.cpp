#include "config/device_config.hpp"
#include "config/parameter_table.hpp"
#include "config/keying_presets.hpp"  // Task 2.8: GetPresetConfig() for factory defaults

#include <algorithm>
#include <array>
#include <cstring>

extern "C" {
#include "esp_log.h"
}

namespace config {

namespace {
constexpr char kLogTag[] = "ConfigStorage";

// Task 3.8: Kept only config_version key - all other keys now in PARAMETER_TABLE
constexpr const char* kKeyConfigVersion = "cfg_version";  // Task 5.5.1

// Task 2.1: Preset customization NVS keys (config v5)
constexpr const char* kKeyPresets = "key_presets";      // Blob: 10 PresetConfigData structs (~640 bytes)
constexpr const char* kKeyManualL = "key_manual_l";     // Manual mode L parameter (uint8_t)
constexpr const char* kKeyManualS = "key_manual_s";     // Manual mode S parameter (uint8_t)
constexpr const char* kKeyManualP = "key_manual_p";     // Manual mode P parameter (uint8_t)

// Template helper for loading simple numeric types from NVS
// Reduces code duplication across INT32, UINT32, UINT16, UINT8 cases
template<typename T, typename NvsFunc>
bool LoadSimpleType(
    nvs_handle_t handle,
    const ParameterDescriptor& desc,
    DeviceConfig& config,
    void* field_ptr,
    NvsFunc nvs_get_func) {

  T value;
  esp_err_t err = nvs_get_func(handle, desc.nvs_key, &value);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return true;  // Use default value from struct initializer
  }

  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "NVS error reading %s: %s", desc.name, esp_err_to_name(err));
    return false;
  }

  if (desc.validator && !desc.validator(&value, config)) {
    ESP_LOGW(kLogTag, "Validation failed for %s", desc.name);
    return false;
  }

  *static_cast<T*>(field_ptr) = value;
  return true;
}

// Template helper for saving simple numeric types to NVS
// Reduces code duplication across INT32, UINT32, UINT16, UINT8 cases
template<typename T, typename NvsFunc>
esp_err_t SaveSimpleType(
    nvs_handle_t handle,
    const char* nvs_key,
    const void* field_ptr,
    NvsFunc nvs_set_func) {
  return nvs_set_func(handle, nvs_key, *static_cast<const T*>(field_ptr));
}

}  // namespace

Storage::~Storage() {
  if (opened_) {
    nvs_close(handle_);
    opened_ = false;
  }
}

esp_err_t Storage::Initialize(const char* nvs_namespace) {
  if (opened_) {
    return ESP_ERR_INVALID_STATE;
  }
  const esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &handle_);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to open NVS namespace '%s': %s", nvs_namespace,
             esp_err_to_name(err));
    return err;
  }
  opened_ = true;
  return ESP_OK;
}

bool Storage::LoadParameter(const ParameterDescriptor& desc, DeviceConfig& config) const {
  void* field_ptr = reinterpret_cast<uint8_t*>(&config) + desc.offset;

  switch (desc.type) {
    case NvsType::INT32:
      return LoadSimpleType<int32_t>(handle_, desc, config, field_ptr, nvs_get_i32);

    case NvsType::UINT32:
      return LoadSimpleType<uint32_t>(handle_, desc, config, field_ptr, nvs_get_u32);

    case NvsType::UINT16:
      return LoadSimpleType<uint16_t>(handle_, desc, config, field_ptr, nvs_get_u16);

    case NvsType::UINT8:
      return LoadSimpleType<uint8_t>(handle_, desc, config, field_ptr, nvs_get_u8);

    case NvsType::INT8: {
      // INT8 stored as UINT8 in NVS, requires conversion
      uint8_t raw_value;
      esp_err_t err = nvs_get_u8(handle_, desc.nvs_key, &raw_value);
      if (err == ESP_ERR_NVS_NOT_FOUND) return true;
      if (err != ESP_OK) {
        ESP_LOGW(kLogTag, "NVS error reading %s: %s", desc.name, esp_err_to_name(err));
        return false;
      }

      int8_t value = static_cast<int8_t>(raw_value);
      if (desc.validator && !desc.validator(&value, config)) {
        ESP_LOGW(kLogTag, "Validation failed for %s", desc.name);
        return false;
      }

      *static_cast<int8_t*>(field_ptr) = value;
      return true;
    }

    case NvsType::BOOL: {
      // BOOL stored as UINT8 in NVS, requires conversion
      uint8_t raw_value;
      esp_err_t err = nvs_get_u8(handle_, desc.nvs_key, &raw_value);
      if (err == ESP_ERR_NVS_NOT_FOUND) return true;
      if (err != ESP_OK) {
        ESP_LOGW(kLogTag, "NVS error reading %s: %s", desc.name, esp_err_to_name(err));
        return false;
      }

      bool value = (raw_value != 0);
      if (desc.validator && !desc.validator(&value, config)) {
        ESP_LOGW(kLogTag, "Validation failed for %s", desc.name);
        return false;
      }

      *static_cast<bool*>(field_ptr) = value;
      return true;
    }

    case NvsType::STRING: {
      char* str_ptr = static_cast<char*>(field_ptr);
      size_t required_size = 0;

      // First call: get size
      esp_err_t err = nvs_get_str(handle_, desc.nvs_key, nullptr, &required_size);
      if (err == ESP_ERR_NVS_NOT_FOUND) return true;  // Use default
      if (err != ESP_OK) {
        ESP_LOGW(kLogTag, "NVS error reading %s: %s", desc.name, esp_err_to_name(err));
        return false;
      }

      // Check size fits in field
      if (required_size > desc.size) {
        ESP_LOGW(kLogTag, "String too large for %s: required=%zu, max=%zu",
                 desc.name, required_size, desc.size);
        return false;
      }

      // Second call: read string
      err = nvs_get_str(handle_, desc.nvs_key, str_ptr, &required_size);
      if (err != ESP_OK) {
        ESP_LOGW(kLogTag, "NVS error reading %s: %s", desc.name, esp_err_to_name(err));
        return false;
      }

      if (desc.validator && !desc.validator(str_ptr, config)) {
        ESP_LOGW(kLogTag, "Validation failed for %s", desc.name);
        return false;
      }

      return true;
    }

    case NvsType::FLOAT: {
      // FLOAT stored as UINT32 in NVS, requires type punning via memcpy
      uint32_t raw_value;
      esp_err_t err = nvs_get_u32(handle_, desc.nvs_key, &raw_value);
      if (err == ESP_ERR_NVS_NOT_FOUND) return true;
      if (err != ESP_OK) {
        ESP_LOGW(kLogTag, "NVS error reading %s: %s", desc.name, esp_err_to_name(err));
        return false;
      }

      float value;
      std::memcpy(&value, &raw_value, sizeof(float));

      if (desc.validator && !desc.validator(&value, config)) {
        ESP_LOGW(kLogTag, "Validation failed for %s (value=%f)", desc.name, value);
        return false;
      }

      *static_cast<float*>(field_ptr) = value;
      return true;
    }

    default:
      ESP_LOGE(kLogTag, "Unknown NvsType for %s", desc.name);
      return false;
  }
}

bool Storage::LoadAllParametersFromTable(DeviceConfig& config) const {
  for (size_t i = 0; i < kParameterCount; ++i) {
    if (!LoadParameter(kParameterDescriptors[i], config)) {
      ESP_LOGW(kLogTag, "Failed to load parameter: %s", kParameterDescriptors[i].name);
      return false;  // All-or-nothing: one failure aborts entire load
    }
  }

  ESP_LOGI(kLogTag, "Successfully loaded %zu parameters from NVS", kParameterCount);
  return true;
}

DeviceConfig Storage::LoadPreviousOrDefaults() {
  // Try backup namespace
  if (HasBackup("keyer_backup")) {
    ESP_LOGI(kLogTag, "Attempting restore from backup namespace");

    Storage backup_storage;
    if (backup_storage.Initialize("keyer_backup") == ESP_OK) {
      DeviceConfig backup_config{};

      if (backup_storage.LoadAllParametersFromTable(backup_config)) {
        ESP_LOGI(kLogTag, "Restored configuration from backup");

        // Restore backup to primary namespace (without creating another backup to prevent recursion)
        if (Save(backup_config, false) == ESP_OK) {
          ESP_LOGI(kLogTag, "Backup restored to primary namespace");
        }

        return backup_config;
      } else {
        ESP_LOGW(kLogTag, "Backup namespace validation failed");
      }
    }
  }

  // Last resort: hard-coded defaults
  ESP_LOGW(kLogTag, "Using hard-coded defaults");
  DeviceConfig defaults{};

  // Persist defaults to NVS so next boot doesn't repeat this fallback
  // Don't create backup since we're already in fallback mode
  if (Save(defaults, false) == ESP_OK) {
    ESP_LOGI(kLogTag, "Default configuration persisted to NVS");
  }

  return defaults;
}

bool Storage::SaveParameter(const ParameterDescriptor& desc, const DeviceConfig& config) {
  const void* field_ptr = reinterpret_cast<const uint8_t*>(&config) + desc.offset;
  esp_err_t err = ESP_OK;

  switch (desc.type) {
    case NvsType::INT32:
      err = SaveSimpleType<int32_t>(handle_, desc.nvs_key, field_ptr, nvs_set_i32);
      break;

    case NvsType::UINT32:
      err = SaveSimpleType<uint32_t>(handle_, desc.nvs_key, field_ptr, nvs_set_u32);
      break;

    case NvsType::UINT16:
      err = SaveSimpleType<uint16_t>(handle_, desc.nvs_key, field_ptr, nvs_set_u16);
      break;

    case NvsType::UINT8:
      err = SaveSimpleType<uint8_t>(handle_, desc.nvs_key, field_ptr, nvs_set_u8);
      break;

    case NvsType::INT8:
      // INT8 stored as UINT8 in NVS
      err = nvs_set_u8(handle_, desc.nvs_key,
                       static_cast<uint8_t>(*static_cast<const int8_t*>(field_ptr)));
      break;

    case NvsType::BOOL:
      // BOOL stored as UINT8 in NVS
      err = nvs_set_u8(handle_, desc.nvs_key,
                       *static_cast<const bool*>(field_ptr) ? 1 : 0);
      break;

    case NvsType::STRING:
      err = nvs_set_str(handle_, desc.nvs_key, static_cast<const char*>(field_ptr));
      break;

    case NvsType::FLOAT: {
      // FLOAT stored as UINT32 in NVS, requires type punning via memcpy
      uint32_t raw_value;
      std::memcpy(&raw_value, field_ptr, sizeof(float));
      err = nvs_set_u32(handle_, desc.nvs_key, raw_value);
      break;
    }

    default:
      ESP_LOGE(kLogTag, "Unknown NvsType for %s", desc.name);
      return false;
  }

  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to persist %s: %s", desc.name, esp_err_to_name(err));
    return false;
  }

  return true;
}

DeviceConfig Storage::LoadWithoutBackupFallback() {
  DeviceConfig config{};

  if (!opened_) {
    ESP_LOGW(kLogTag, "Storage not initialized; returning defaults.");
    return config;
  }

  // Load and check configuration version
  uint32_t stored_version = 0;
  esp_err_t err = nvs_get_u32(handle_, kKeyConfigVersion, &stored_version);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kLogTag, "No stored config version found, assuming version 0 (pre-versioning)");
    stored_version = 0;
  } else if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Failed to read config version: %s, returning defaults (no backup fallback)", esp_err_to_name(err));
    return config;  // Return defaults WITHOUT attempting backup restore
  }

  config.general.config_version = stored_version;

  // Load all parameters (all-or-nothing)
  if (!LoadAllParametersFromTable(config)) {
    ESP_LOGW(kLogTag, "Parameter load/validation failed, returning defaults (no backup fallback)");
    return config;  // Return defaults WITHOUT attempting backup restore
  }

  // Apply migration if needed
  if (stored_version < kCurrentConfigVersion) {
    ESP_LOGI(kLogTag, "Migrating config from version %lu to %lu",
             static_cast<unsigned long>(stored_version),
             static_cast<unsigned long>(kCurrentConfigVersion));
    MigrateConfig(config, stored_version);
    config.general.config_version = kCurrentConfigVersion;
  }

  // Resolve GPIO conflicts: key_gpio and trx_gpio cannot use same pin
  if (config.paddle_pins.key_gpio >= 0 &&
      config.paddle_pins.key_gpio == config.output_pins.trx_gpio) {
    ESP_LOGW(kLogTag,
             "GPIO conflict detected: key_gpio (%d) == trx_gpio (%d), disabling key_gpio",
             static_cast<int>(config.paddle_pins.key_gpio),
             static_cast<int>(config.output_pins.trx_gpio));
    config.paddle_pins.key_gpio = -1;  // Disable straight key input to avoid conflict
  }

  return config;
}

DeviceConfig Storage::LoadOrDefault() {
  DeviceConfig config{};

  if (!opened_) {
    ESP_LOGW(kLogTag, "Storage not initialized; returning defaults.");
    return config;
  }

  // Load and check configuration version
  uint32_t stored_version = 0;
  esp_err_t err = nvs_get_u32(handle_, kKeyConfigVersion, &stored_version);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kLogTag, "No stored config version found, assuming version 0 (pre-versioning)");
    stored_version = 0;
  } else if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Failed to read config version: %s, falling back", esp_err_to_name(err));
    return LoadPreviousOrDefaults();
  }

  config.general.config_version = stored_version;

  // Load all parameters (all-or-nothing)
  if (!LoadAllParametersFromTable(config)) {
    ESP_LOGW(kLogTag, "Parameter load/validation failed, attempting fallback");
    return LoadPreviousOrDefaults();
  }

  // Task 2.7: Load preset customization data (config v5)
  // Must load BEFORE migration so v4→v5 migration can access presets
  if (!LoadPresets(config)) {
    ESP_LOGW(kLogTag, "Failed to load presets (non-critical, using factory defaults)");
    // Non-critical: LoadPresets() always falls back to factory defaults
  }

  if (!LoadManualLSP(config)) {
    ESP_LOGW(kLogTag, "Failed to load Manual L-S-P (non-critical, using defaults)");
    // Non-critical: LoadManualLSP() always falls back to defaults (30-50-50)
  }

  // Apply migration if needed
  if (stored_version < kCurrentConfigVersion) {
    ESP_LOGI(kLogTag, "Migrating config from version %lu to %lu",
             static_cast<unsigned long>(stored_version),
             static_cast<unsigned long>(kCurrentConfigVersion));
    MigrateConfig(config, stored_version);
    config.general.config_version = kCurrentConfigVersion;

    // Save migrated configuration to NVS (Task 2.7: v4→v5 migration persistence)
    ESP_LOGI(kLogTag, "Saving migrated configuration to NVS");
    esp_err_t save_err = Save(config, false);  // Don't create backup during migration save
    if (save_err != ESP_OK) {
      ESP_LOGW(kLogTag, "Failed to save migrated config: %s (non-critical)", esp_err_to_name(save_err));
    }
  }

  // Resolve GPIO conflicts: key_gpio and trx_gpio cannot use same pin
  if (config.paddle_pins.key_gpio >= 0 &&
      config.paddle_pins.key_gpio == config.output_pins.trx_gpio) {
    ESP_LOGW(kLogTag,
             "GPIO conflict detected: key_gpio (%d) == trx_gpio (%d), disabling key_gpio",
             static_cast<int>(config.paddle_pins.key_gpio),
             static_cast<int>(config.output_pins.trx_gpio));
    config.paddle_pins.key_gpio = -1;  // Disable straight key input to avoid conflict
  }

  return config;
}

esp_err_t Storage::Save(const DeviceConfig& config, bool create_backup) {
  if (!opened_) {
    return ESP_ERR_INVALID_STATE;
  }

  // Task 3.6: Create automatic backup before saving (unless disabled to prevent recursion)
  // This backup is used by LoadPreviousOrDefaults() if load fails
  //
  // WORKAROUND for ESP-IDF NVS bug: After erase_flash, writing to a newly created namespace
  // causes LoadProhibited/StoreProhibited crashes due to uninitialized internal structures.
  // Solution: Only create backup if the backup namespace already exists (e.g., from previous firmware).
  // After erase_flash, backup will be disabled until namespace is manually created or ESP-IDF fixes the bug.
  if (create_backup && HasBackup("keyer_backup")) {
    esp_err_t backup_err = Backup("keyer_backup", &config);
    if (backup_err != ESP_OK) {
      ESP_LOGW(kLogTag, "Failed to create backup before save: %s (proceeding anyway)",
               esp_err_to_name(backup_err));
      // Continue with save even if backup fails - better to save new config than abort
    }
  } else if (create_backup) {
    ESP_LOGI(kLogTag, "Backup namespace does not exist, skipping backup (NVS bug workaround after erase_flash)");
  }

  // Task 3.8: Save config_version separately (not in PARAMETER_TABLE)
  esp_err_t err = nvs_set_u32(handle_, kKeyConfigVersion, config.general.config_version);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to persist config version: %s", esp_err_to_name(err));
    return err;
  }

  // Task 3.8: Save all parameters using table-driven approach
  for (size_t i = 0; i < kParameterCount; ++i) {
    if (!SaveParameter(kParameterDescriptors[i], config)) {
      ESP_LOGE(kLogTag, "Failed to save parameter: %s", kParameterDescriptors[i].name);
      return ESP_FAIL;
    }
  }

  // Task 2.6: Save preset customization data (config v5)
  if (!SavePresets(config)) {
    ESP_LOGW(kLogTag, "Failed to save presets (non-critical, proceeding with commit)");
    // Non-critical: Continue with commit even if preset save fails
  }

  if (!SaveManualLSP(config)) {
    ESP_LOGW(kLogTag, "Failed to save Manual L-S-P (non-critical, proceeding with commit)");
    // Non-critical: Continue with commit even if Manual L-S-P save fails
  }

  // Commit all changes to NVS
  const esp_err_t commit_err = nvs_commit(handle_);
  if (commit_err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to commit configuration: %s", esp_err_to_name(commit_err));
  }
  return commit_err;
}

// Task 3.8: All Save*Config methods removed - replaced by SaveParameter() loop in Save()

esp_err_t Storage::Backup(const char* backup_namespace, const DeviceConfig* config_to_backup) {
  if (!opened_) {
    ESP_LOGE(kLogTag, "Storage not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(kLogTag, "Backing up configuration to namespace '%s'", backup_namespace);

  // Use provided config or load current config WITHOUT attempting backup restore
  // (prevents infinite recursion if backup is corrupted and reduces stack usage)
  DeviceConfig local_config{};
  const DeviceConfig* config_ptr = config_to_backup;

  if (config_ptr == nullptr) {
    local_config = LoadWithoutBackupFallback();
    config_ptr = &local_config;
  }

  // Open backup namespace for writing
  // Note: nvs_set_*() operations overwrite existing values, so no need to erase first.
  // Previously tried nvs_erase_all() here, but it causes LoadProhibited crashes when
  // called on newly created (empty) namespaces due to uninitialized internal structures.
  nvs_handle_t backup_handle = 0;
  esp_err_t err = nvs_open(backup_namespace, NVS_READWRITE, &backup_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to open backup namespace '%s': %s",
             backup_namespace, esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(kLogTag, "Opened backup namespace for writing");

  // Temporarily swap handle to write to backup namespace
  nvs_handle_t original_handle = handle_;
  bool original_opened = opened_;

  handle_ = backup_handle;
  opened_ = true;

  // Save without creating another backup to prevent infinite recursion
  err = Save(*config_ptr, false);

  // Restore original handle and opened flag
  handle_ = original_handle;
  opened_ = original_opened;
  nvs_close(backup_handle);

  if (err == ESP_OK) {
    ESP_LOGI(kLogTag, "Configuration backed up successfully");
  } else {
    ESP_LOGE(kLogTag, "Failed to backup configuration: %s", esp_err_to_name(err));
  }

  return err;
}

// Task 5.5.2: Restore configuration from backup namespace

esp_err_t Storage::Restore(const char* backup_namespace) {
  if (!opened_) {
    ESP_LOGE(kLogTag, "Storage not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(kLogTag, "Restoring configuration from namespace '%s'", backup_namespace);

  // Open backup namespace
  nvs_handle_t backup_handle = 0;
  esp_err_t err = nvs_open(backup_namespace, NVS_READONLY, &backup_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to open backup namespace '%s': %s",
             backup_namespace, esp_err_to_name(err));
    return err;
  }

  // Load config from backup
  nvs_handle_t original_handle = handle_;
  bool original_opened = opened_;

  handle_ = backup_handle;
  opened_ = true;

  DeviceConfig config = LoadOrDefault();

  handle_ = original_handle;
  opened_ = original_opened;
  nvs_close(backup_handle);

  // Save to current namespace (disable backup to prevent recursion)
  err = Save(config, false);

  if (err == ESP_OK) {
    ESP_LOGI(kLogTag, "Configuration restored successfully");
  } else {
    ESP_LOGE(kLogTag, "Failed to restore configuration: %s", esp_err_to_name(err));
  }

  return err;
}

// Task 5.5.2: Clone configuration between namespaces
esp_err_t Storage::Clone(const char* source_namespace, const char* dest_namespace) {
  ESP_LOGI(kLogTag, "Cloning configuration from '%s' to '%s'",
           source_namespace, dest_namespace);

  // Open source namespace
  nvs_handle_t source_handle = 0;
  esp_err_t err = nvs_open(source_namespace, NVS_READONLY, &source_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to open source namespace '%s': %s",
             source_namespace, esp_err_to_name(err));
    return err;
  }

  // Load config from source
  nvs_handle_t original_handle = handle_;
  bool original_opened = opened_;

  handle_ = source_handle;
  opened_ = true;

  DeviceConfig config = LoadOrDefault();

  handle_ = original_handle;
  opened_ = original_opened;
  nvs_close(source_handle);

  // Open destination namespace
  nvs_handle_t dest_handle = 0;
  err = nvs_open(dest_namespace, NVS_READWRITE, &dest_handle);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to open dest namespace '%s': %s",
             dest_namespace, esp_err_to_name(err));
    return err;
  }

  // Save to destination
  handle_ = dest_handle;
  opened_ = true;

  // Save without creating a backup to prevent infinite recursion
  err = Save(config, false);

  handle_ = original_handle;
  opened_ = original_opened;
  nvs_close(dest_handle);

  if (err == ESP_OK) {
    ESP_LOGI(kLogTag, "Configuration cloned successfully");
  } else {
    ESP_LOGE(kLogTag, "Failed to clone configuration: %s", esp_err_to_name(err));
  }

  return err;
}

// Task 5.5.2: Check if backup exists
bool Storage::HasBackup(const char* backup_namespace) const {
  nvs_handle_t backup_handle = 0;
  esp_err_t err = nvs_open(backup_namespace, NVS_READONLY, &backup_handle);
  if (err != ESP_OK) {
    return false;
  }

  // Check if version key exists as indicator of valid backup
  uint32_t version = 0;
  err = nvs_get_u32(backup_handle, kKeyConfigVersion, &version);
  nvs_close(backup_handle);

  return (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND);  // Accept both versioned and pre-version backups
}

// Task 5.4.0.8: Apply WiFi secrets from wifi_secrets.h if NVS is empty
esp_err_t Storage::ApplyWiFiSecretsIfEmpty() {
  if (!opened_) {
    return ESP_ERR_INVALID_STATE;
  }

  // Check if WiFi STA SSID exists in NVS
  size_t required_size = 0;
  esp_err_t err = nvs_get_str(handle_, "wifi_sta_ssid", nullptr, &required_size);

  if (err == ESP_OK && required_size > 1) {
    // WiFi config already exists in NVS, don't overwrite
    ESP_LOGI(kLogTag, "WiFi config already in NVS, skipping wifi_secrets.h defaults");
    return ESP_OK;
  }

  // NVS is empty, apply defaults from DeviceConfig (which includes wifi_secrets.h)
  DeviceConfig defaults{};  // This gets defaults from struct initializer

  // Only apply if wifi_secrets.h provided non-empty values
  if (defaults.wifi.sta_ssid[0] == '\0') {
    ESP_LOGI(kLogTag, "No WiFi secrets defined (wifi_secrets.h not configured)");
    return ESP_OK;
  }

  ESP_LOGI(kLogTag, "Applying WiFi defaults from wifi_secrets.h to NVS (SSID: %s)",
           defaults.wifi.sta_ssid);

  // Task 3.8: Use table-driven Save() instead of SaveWiFiConfig()
  // Disable backup during initialization to prevent NVS crash (opening second namespace during init)
  err = Save(defaults, false);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to save WiFi secrets to NVS: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(kLogTag, "WiFi secrets successfully written to NVS");
  return ESP_OK;
}

// Task 5.5.1: Configuration migration
void Storage::MigrateConfig(DeviceConfig& config, uint32_t from_version) const {
  ESP_LOGI(kLogTag, "Applying config migrations from version %lu", static_cast<unsigned long>(from_version));

  // Version 0 -> 1: No structural changes in initial version
  if (from_version < 1) {
    ESP_LOGI(kLogTag, "Migrating from version 0 to 1 (no changes)");
  }

  // Version 1 -> 2: Added keying.swap_paddles field
  if (from_version < 2) {
    ESP_LOGI(kLogTag, "Migrating from version 1 to 2: initializing swap_paddles=false");
    config.keying.swap_paddles = false;  // Ensure new field has correct default
  }

  // Version 2 -> 3: Added StoredMessagesConfig (10 messages: message1-message10)
  if (from_version < 3) {
    ESP_LOGI(kLogTag, "Migrating from version 2 to 3: initializing stored_messages");
    // Initialize message strings with defaults (these will be used if not found in NVS)
    std::strncpy(config.stored_messages.message1, "CQ CQ CQ DE", sizeof(config.stored_messages.message1) - 1);
    std::strncpy(config.stored_messages.message2, "TEST DE", sizeof(config.stored_messages.message2) - 1);
    std::strncpy(config.stored_messages.message3, "TU 73", sizeof(config.stored_messages.message3) - 1);
    std::strncpy(config.stored_messages.message4, "QRZ?", sizeof(config.stored_messages.message4) - 1);
    config.stored_messages.message5[0] = '\0';  // Empty
    config.stored_messages.message6[0] = '\0';  // Empty
    config.stored_messages.message7[0] = '\0';  // Empty
    config.stored_messages.message8[0] = '\0';  // Empty
    config.stored_messages.message9[0] = '\0';  // Empty
    config.stored_messages.message10[0] = '\0'; // Empty
  }

  // Version 3 -> 4: Added remote audio streaming configuration (stream_audio, stream_volume)
  if (from_version < 4) {
    ESP_LOGI(kLogTag, "Migrating from version 3 to 4: initializing remote audio streaming params");
    config.remote.stream_audio = false;   // Default: disabled for backward compatibility
    config.remote.stream_volume = 100;    // Default: full volume (preserves current behavior when enabled)
  }

  // Version 4 -> 5: Added per-preset L-S-P timing parameters (preset_definitions[10] array)
  if (from_version < 5) {
    ESP_LOGI(kLogTag, "Migrating from version 4 to 5: migrating global L-S-P to per-preset");

    // Step 1: Read old global L-S-P keys (if present, defaults: 30-50-50)
    // Note: These keys were NEVER saved to NVS in v4 (runtime-only), but check anyway
    uint8_t old_l = 30;
    uint8_t old_s = 50;
    uint8_t old_p = 50;

    // Attempt to read old keys (may not exist if never customized)
    esp_err_t err_l = nvs_get_u8(handle_, "key_timing_l", &old_l);
    esp_err_t err_s = nvs_get_u8(handle_, "key_timing_s", &old_s);
    esp_err_t err_p = nvs_get_u8(handle_, "key_timing_p", &old_p);

    if (err_l == ESP_OK || err_s == ESP_OK || err_p == ESP_OK) {
      ESP_LOGI(kLogTag, "Found old L-S-P values: L=%u, S=%u, P=%u", old_l, old_s, old_p);
    } else {
      ESP_LOGI(kLogTag, "No old L-S-P keys found, using defaults: L=30, S=50, P=50");
    }

    // Step 2: Initialize all 10 presets from factory defaults
    InitializePresetsFromDefaults(config);
    ESP_LOGI(kLogTag, "Initialized preset_definitions[10] from factory defaults");

    // Step 3: Apply old global L-S-P to V9 preset (migration target)
    config.keying.preset_definitions[9].timing_l = old_l;
    config.keying.preset_definitions[9].timing_s = old_s;
    config.keying.preset_definitions[9].timing_p = old_p;
    ESP_LOGI(kLogTag, "Applied old L-S-P to V9 preset: L=%u, S=%u, P=%u", old_l, old_s, old_p);

    // Step 4: Apply old global L-S-P to Manual mode working copy
    config.keying.timing_l = old_l;
    config.keying.timing_s = old_s;
    config.keying.timing_p = old_p;
    ESP_LOGI(kLogTag, "Applied old L-S-P to Manual mode: L=%u, S=%u, P=%u", old_l, old_s, old_p);

    // Step 5: Save migrated data to NVS
    // Note: Can't call SavePresets/SaveManualLSP here (MigrateConfig is const)
    // These will be saved by caller (LoadOrDefault) via automatic Save after migration

    // Step 6: Delete old NVS keys (cleanup)
    nvs_erase_key(handle_, "key_timing_l");  // Ignore errors (key may not exist)
    nvs_erase_key(handle_, "key_timing_s");
    nvs_erase_key(handle_, "key_timing_p");
    ESP_LOGI(kLogTag, "Deleted old L-S-P keys from NVS");

    ESP_LOGI(kLogTag, "Migration v4->v5 completed successfully");
  }

  ESP_LOGI(kLogTag, "Migration completed");
}

// ============================================================================
// Task 2.0: Preset Customization Storage Methods
// ============================================================================

/**
 * @brief Save preset_definitions[10] array to NVS as blob
 *
 * Saves all 10 preset configurations (V0-V9) to NVS. kManual (255) does NOT
 * use this array - Manual mode uses timing_l/s/p fields saved separately.
 *
 * @param config Configuration containing preset_definitions[10] array
 * @return true on success, false on NVS error (graceful degradation)
 *
 * @note Task 2.2: Anti-bootloop design
 *       - Returns false on error (no abort())
 *       - Logs error but allows firmware to continue
 *       - Blob size: 640 bytes (10 × 64 bytes per PresetConfigData)
 */
bool Storage::SavePresets(const DeviceConfig& config) {
  constexpr size_t kBlobSize = sizeof(config.keying.preset_definitions);  // ~640 bytes

  esp_err_t err = nvs_set_blob(handle_, kKeyPresets,
                               config.keying.preset_definitions,
                               kBlobSize);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to save presets blob (%zu bytes): %s",
             kBlobSize, esp_err_to_name(err));
    return false;
  }

  ESP_LOGD(kLogTag, "Saved %zu bytes of preset definitions to NVS", kBlobSize);
  return true;
}

/**
 * @brief Load preset_definitions[10] array from NVS blob
 *
 * Loads all 10 preset configurations (V0-V9) from NVS. If NVS key not found
 * or read fails, initializes from factory defaults (fallback for anti-bootloop).
 *
 * @param config Configuration to populate with preset_definitions[10]
 * @return true on success OR graceful fallback, false only on critical error
 *
 * @note Task 2.3: Anti-bootloop design
 *       - ESP_ERR_NVS_NOT_FOUND → Initialize from factory defaults
 *       - Size mismatch → Warn and initialize from factory defaults
 *       - Other errors → Warn and initialize from factory defaults
 *       - Never returns false (always ensures valid preset data)
 */
bool Storage::LoadPresets(DeviceConfig& config) {
  constexpr size_t kExpectedSize = sizeof(config.keying.preset_definitions);
  size_t actual_size = kExpectedSize;

  esp_err_t err = nvs_get_blob(handle_, kKeyPresets,
                               config.keying.preset_definitions,
                               &actual_size);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kLogTag, "Presets blob not found in NVS, initializing from factory defaults");
    InitializePresetsFromDefaults(config);
    return true;  // Graceful fallback
  }

  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Failed to load presets blob: %s (falling back to factory defaults)",
             esp_err_to_name(err));
    InitializePresetsFromDefaults(config);
    return true;  // Graceful fallback
  }

  if (actual_size != kExpectedSize) {
    ESP_LOGW(kLogTag, "Presets blob size mismatch: expected=%zu, actual=%zu (falling back to factory defaults)",
             kExpectedSize, actual_size);
    InitializePresetsFromDefaults(config);
    return true;  // Graceful fallback
  }

  ESP_LOGD(kLogTag, "Loaded %zu bytes of preset definitions from NVS", actual_size);
  return true;
}

/**
 * @brief Save Manual mode L-S-P parameters to NVS
 *
 * Saves timing_l, timing_s, timing_p to separate NVS keys for Manual mode persistence.
 * These parameters persist across reboots when preset == kManual.
 *
 * @param config Configuration containing timing_l/s/p values
 * @return true on success, false on NVS error (graceful degradation)
 *
 * @note Task 2.4: Manual mode independence
 *       - Separate keys from preset_definitions[] array
 *       - Only loaded when preset == kManual
 */
bool Storage::SaveManualLSP(const DeviceConfig& config) {
  esp_err_t err_l = nvs_set_u8(handle_, kKeyManualL, config.keying.timing_l);
  esp_err_t err_s = nvs_set_u8(handle_, kKeyManualS, config.keying.timing_s);
  esp_err_t err_p = nvs_set_u8(handle_, kKeyManualP, config.keying.timing_p);

  if (err_l != ESP_OK || err_s != ESP_OK || err_p != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to save Manual L-S-P: L=%s, S=%s, P=%s",
             esp_err_to_name(err_l), esp_err_to_name(err_s), esp_err_to_name(err_p));
    return false;
  }

  ESP_LOGD(kLogTag, "Saved Manual L-S-P: L=%u, S=%u, P=%u",
           config.keying.timing_l, config.keying.timing_s, config.keying.timing_p);
  return true;
}

/**
 * @brief Load Manual mode L-S-P parameters from NVS
 *
 * Loads timing_l/s/p from separate NVS keys. If keys not found, keeps default
 * values (30-50-50) from struct initializer.
 *
 * @param config Configuration to populate with timing_l/s/p
 * @return true on success OR graceful fallback (always succeeds)
 *
 * @note Task 2.5: Anti-bootloop design
 *       - ESP_ERR_NVS_NOT_FOUND → Use defaults (30-50-50)
 *       - Never returns false (always ensures valid data)
 */
bool Storage::LoadManualLSP(DeviceConfig& config) {
  uint8_t l, s, p;

  esp_err_t err_l = nvs_get_u8(handle_, kKeyManualL, &l);
  esp_err_t err_s = nvs_get_u8(handle_, kKeyManualS, &s);
  esp_err_t err_p = nvs_get_u8(handle_, kKeyManualP, &p);

  if (err_l == ESP_ERR_NVS_NOT_FOUND || err_s == ESP_ERR_NVS_NOT_FOUND || err_p == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kLogTag, "Manual L-S-P not found in NVS, using defaults (30-50-50)");
    return true;  // Keep struct defaults
  }

  if (err_l != ESP_OK || err_s != ESP_OK || err_p != ESP_OK) {
    ESP_LOGW(kLogTag, "Failed to load Manual L-S-P: L=%s, S=%s, P=%s (using defaults)",
             esp_err_to_name(err_l), esp_err_to_name(err_s), esp_err_to_name(err_p));
    return true;  // Keep struct defaults
  }

  config.keying.timing_l = l;
  config.keying.timing_s = s;
  config.keying.timing_p = p;

  ESP_LOGD(kLogTag, "Loaded Manual L-S-P: L=%u, S=%u, P=%u", l, s, p);
  return true;
}

/**
 * @brief Initialize preset_definitions[10] from factory defaults
 *
 * Copies factory default PresetConfigData (from kPresetConfigurations in
 * keying_presets.cpp) into config.keying.preset_definitions[10] array.
 *
 * @param config Configuration to populate with factory preset definitions
 *
 * @note Task 2.8: Factory defaults source
 *       - Data source: keying_presets.cpp::kPresetConfigurations[]
 *       - Called on first boot or NVS corruption
 *       - Called during v4→v5 migration
 */
void Storage::InitializePresetsFromDefaults(DeviceConfig& config) {
  // Use GetPresetConfig() to retrieve factory defaults for each preset
  for (uint8_t i = 0; i < 10; ++i) {
    KeyingPreset preset = static_cast<KeyingPreset>(i);
    config.keying.preset_definitions[i] = GetPresetConfig(preset);
  }

  ESP_LOGI(kLogTag, "Initialized %d presets from factory defaults", 10);
}

}  // namespace config
