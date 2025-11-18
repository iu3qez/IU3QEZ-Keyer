#pragma once

/**
 * @file device_config.hpp
 * @brief Central device configuration structures and NVS persistence layer
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * This file defines the **single source of truth** for all device configuration.
 * Follows a hierarchical struct-based design with zero-initialization guarantees
 * and explicit default values for hardware bootstrap.
 *
 * DESIGN PRINCIPLES:
 * ==================
 * 1. **Aggregate Initialization**: All config structs use C++11 member initializers
 *    for predictable defaults (e.g., `int32_t dit_gpio = 3;`)
 * 2. **Type Safety**: Enums (KeyingPreset) prevent invalid preset values at compile-time
 * 3. **GPIO Flexibility**: Pin assignments use int32_t (-1 = disabled) for optional pins
 * 4. **Separation of Concerns**: Hardware config (pins, addresses) isolated from
 *    runtime parameters (WPM, preset, volume)
 * 5. **NVS Persistence**: Storage class provides Load/Save with automatic migration
 *
 * CONFIGURATION STRUCTURE:
 * ========================
 * DeviceConfig (root)
 *  ├─ GeneralConfig: Callsign, version
 *  ├─ PaddlePins: Dit/Dah/Key GPIO, polarity, pullups/pulldowns
 *  ├─ OutputPins: TX output GPIO, active polarity
 *  ├─ NeoPixelConfig: WS2812 GPIO, LED count
 *  ├─ I2cPins: SDA/SCL GPIO (shared bus for codec + IO expander)
 *  ├─ I2sPins: MCLK/BCLK/LRCK/DOUT (ES8311 audio codec interface)
 *  ├─ CodecConfig: ES8311 I2C address (0x18)
 *  ├─ IoExpanderConfig: TCA9555 I2C address, USB selector pin, PA enable pin
 *  ├─ AudioConfig: Sidetone frequency, volume, fade in/out, enabled flag
 *  ├─ KeyingConfig: Preset (V0-V9/Manual), WPM, memory windows, late release
 *  └─ WiFiConfig: STA SSID/password, AP SSID/password, fallback timeout
 *
 * HARDWARE DEFAULTS:
 * ==================
 * Default GPIO assignments optimized for ESP32-S3 development board:
 * - Paddle inputs: GPIO 3 (dit), GPIO 4 (dah)
 * - TX output: GPIO 15 (active-high)
 * - NeoPixel: GPIO 38 (7 LEDs)
 * - I2C: GPIO 10 (SCL), GPIO 11 (SDA)
 * - I2S: GPIO 12 (MCLK), GPIO 13 (BCLK), GPIO 14 (LRCK), GPIO 16 (DOUT)
 *
 * KEYING PRESETS:
 * ===============
 * Enum KeyingPreset defines 10 preconfigured iambic modes:
 * - V0-V2: SuperKeyer (dot+dash/dot-only/dash-only memory)
 * - V3-V5: Accukeyer (dot+dash/dot-only/dash-only memory)
 * - V6-V8: Curtis Mode A (dot+dash/dot-only/dash-only memory)
 * - V9: No memory (plain iambic, state-latch disabled)
 * - Manual (255): User-configurable memory windows and latch mode
 *
 * NVS STORAGE:
 * ============
 * Storage class provides Load/Save/Delete operations with:
 * - Automatic migration for config_version changes
 * - Per-subsystem blob storage (audio, keying, wifi, etc.)
 * - Graceful fallback to defaults on corruption/missing keys
 * - Explicit namespace isolation ("keyer" by default)
 *
 * USAGE EXAMPLE:
 * ==============
 * @code
 * config::Storage storage;
 * storage.Initialize("keyer");
 *
 * config::DeviceConfig cfg = storage.LoadOrDefault();
 * cfg.keying.speed_wpm = 25;
 * cfg.audio.sidetone_frequency_hz = 700;
 * storage.Save(cfg);  // Persist to NVS
 * @endcode
 *
 * HOT-RELOAD SUPPORT:
 * ===================
 * Runtime-changeable parameters (keying, audio) can be updated via
 * ApplicationController::ApplyConfigChanges() without device reset.
 * Hardware parameters (GPIO pins, I2C/I2S config) require device reset
 * and show warning in Web UI (see parameter_registry.cpp SetRequiresReset).
 *
 * THREAD SAFETY:
 * ==============
 * DeviceConfig struct is **not** thread-safe. Concurrent access from multiple
 * threads requires external synchronization (e.g., mutex or copy-on-write).
 * Storage class assumes single-threaded access during Load/Save operations.
 *
 * DEPENDENCIES:
 * =============
 * - ESP-IDF NVS: Non-volatile storage backend (components/nvs_flash)
 * - wifi_secrets.h: Optional compile-time WiFi credentials (see .h.example)
 * - driver/gpio.h: GPIO type definitions (gpio_num_t)
 */

#include <cstdint>

extern "C" {
#include "driver/gpio.h"
#include "esp_err.h"
#include "nvs.h"
}

// WiFi development credentials (optional - see wifi_secrets.h.example)
// Include wifi_secrets.h only if it exists
#if __has_include("config/wifi_secrets.h")
#include "config/wifi_secrets.h"
#endif

namespace config {

enum class KeyingPreset : uint8_t {
  kSuperKeyerBoth = 0,
  kSuperKeyerDot = 1,
  kSuperKeyerDash = 2,
  kAccukeyerBoth = 3,
  kAccukeyerDot = 4,
  kAccukeyerDash = 5,
  kCurtisABoth = 6,
  kCurtisADot = 7,
  kCurtisADash = 8,
  kNoMemory = 9,
  kManual = 255,
};

struct PaddlePins {
  int32_t dit_gpio = 3;
  int32_t dah_gpio = 4;
  int32_t key_gpio = -1;  // Straight key input (optional, -1 = not configured)
  bool paddles_active_low = true;
  bool use_pullups = true;
  bool use_pulldowns = false;
};

struct OutputPins {
  int32_t trx_gpio = 15;  // Transmitter key output (controlled by iambic logic) - GPIO 15
  bool trx_active_high = true;  // true = HIGH when transmitting, false = LOW when transmitting
};

struct NeoPixelConfig {
  int32_t gpio = 38;
  int32_t led_count = 7;
};

struct I2cPins {
  int32_t sda_gpio = 11;
  int32_t scl_gpio = 10;
};

struct I2sPins {
  int32_t mclk_gpio = 12;
  int32_t bclk_gpio = 13;
  int32_t lrck_gpio = 14;
  int32_t dout_gpio = 16;
};

struct CodecConfig {
  uint8_t i2c_address = 0x18;
};

struct IoExpanderConfig {
  uint8_t i2c_address = 0x20;
  int8_t usb_selector_pin = -1;
  int8_t pa_enable_pin = 8;  // EXI08 on IO expander for PA enable
};

struct AudioConfig {
  uint16_t sidetone_frequency_hz = 600;
  uint8_t sidetone_volume_percent = 50;
  uint16_t sidetone_fade_in_ms = 8;
  uint16_t sidetone_fade_out_ms = 8;
  bool sidetone_enabled = false;
};

/**
 * @brief Preset configuration data structure (8 fields)
 *
 * Defines all timing and behavior parameters for a single keying preset.
 * This is the authoritative source for preset behavior - any subsystem
 * needing preset configuration should query this data.
 *
 * STRUCTURE EVOLUTION:
 * - Original (5 fields): Memory window parameters and trigger mode
 * - Extended (8 fields): Added per-preset L-S-P timing parameters (v5)
 *
 * USAGE:
 * - Factory defaults: kPresetConfigurations[] in keying_presets.cpp
 * - User customization: preset_definitions[] in KeyingConfig (NVS storage)
 * - Runtime application: GetPresetConfig() returns this type
 *
 * Total size: ~64 bytes per preset (8 fields + padding)
 */
struct PresetConfigData {
  // Memory window parameters (0-100%)
  float memory_open_percent;     ///< Window opens at this % of element duration
  float memory_close_percent;    ///< Window closes at this % of element duration

  // Memory enable flags
  bool enable_dit_memory;        ///< True = dit memory enabled
  bool enable_dah_memory;        ///< True = dah memory enabled
  bool use_state_latch;          ///< True = state-latch (Accukeyer), false = edge-trigger (Curtis A)

  // L-S-P timing parameters (per-preset customization, added in config v5)
  uint8_t timing_l = 30;         ///< Dash length ratio (10-90, default 30 = 3:1 ratio). Formula: dash = (L/10.0) * dit
  uint8_t timing_s = 50;         ///< Gap space ratio (0-99, default 50 = 1:1 ratio). Formula: gap = (S/50.0) * dit
  uint8_t timing_p = 50;         ///< Dit duration percentage (10-99, default 50 = 100%). Formula: dit_effective = dit_theoretical * (P/50.0)
};

struct KeyingConfig {
  KeyingPreset preset = KeyingPreset::kAccukeyerBoth;
  uint32_t speed_wpm = 20;
  float memory_open_percent = 60.0f;
  float memory_close_percent = 99.0f;
  bool manual_memory_enable_dit = true;
  bool manual_memory_enable_dah = true;
  bool manual_use_state_latch = true;
  bool swap_paddles = false;  // Swap dit and dah GPIO assignments (for left-handed operators)
  bool decoder_enabled = true;  // Enable morse code decoder (Phase 2)

  // L-S-P timing system (Linea-Spazio-Punto) - Industry-standard HST/QRQ timing parameters
  // Manual mode L-S-P parameters (saved to NVS with keys: key_manual_l, key_manual_s, key_manual_p)
  // When preset == kManual, these values are loaded from NVS and persist across reboots
  // When preset != kManual, these values are runtime-only and reset on reboot
  uint8_t timing_l = 30;  // L (Dash): 10-90, default 30 (3:1 ratio)
  uint8_t timing_s = 50;  // S (Gap): 0-99, default 50 (1:1 ratio)
  uint8_t timing_p = 50;  // P (Dit): 10-99, default 50 (100% theoretical)

  // Per-preset customization storage (NVS key: key_presets)
  // Array indexed 0-9 for V0-V9 presets (~640 bytes: 10 × ~64 bytes)
  // NOTE: kManual (255) does NOT use this array - Manual mode uses timing_l/s/p fields directly
  PresetConfigData preset_definitions[10]{};
};

struct GeneralConfig {
  char callsign[16] = "IU3QEZ";  // Station callsign displayed in status bar and logs
  uint32_t config_version = 5;   // Configuration version for migration support (v5: per-preset L-S-P timing parameters)
};

struct WiFiConfig {
#ifdef WIFI_DEV_SSID
  char sta_ssid[32] = WIFI_DEV_SSID;  // Development default from wifi_secrets.h
#else
  char sta_ssid[32] = "";             // Station mode SSID (empty = use Web UI)
#endif

#ifdef WIFI_DEV_PASSWORD
  char sta_password[64] = WIFI_DEV_PASSWORD;  // Development default from wifi_secrets.h
#else
  char sta_password[64] = "";                 // Station mode password
#endif

#ifdef WIFI_AP_SSID
  char ap_ssid[32] = WIFI_AP_SSID;    // AP SSID from wifi_secrets.h
#else
  char ap_ssid[32] = "Keyer-AP";      // Access Point mode SSID (fallback)
#endif

#ifdef WIFI_AP_PASSWORD
  char ap_password[64] = WIFI_AP_PASSWORD;  // AP password from wifi_secrets.h
#else
  char ap_password[64] = "";                // AP password (empty = open network)
#endif

  bool enable_ap_fallback = true;     // Enable AP mode if STA connection fails
  uint16_t sta_timeout_sec = 30;      // STA connection timeout before fallback to AP
};

struct RemoteConfig {
  bool enabled = false;                // Enable CWNet remote keying client
  char server_host[64] = "";           // CWNet server hostname or IP address
  uint16_t server_port = 7355;         // CWNet server TCP port (default per protocol spec)
  bool auto_reconnect = true;          // Automatically reconnect on connection loss
  uint32_t ptt_tail_ms = 200;          // Base PTT tail delay (network latency added dynamically)
  bool stream_audio = false;           // Enable remote audio streaming (RX mode)
  uint8_t stream_volume = 100;         // Remote audio stream volume (0-100%, default 100)
};

struct ServerConfig {
  bool enabled = false;                // Enable CWNet server for receiving remote keying
  uint16_t listen_port = 7355;         // TCP port to listen on (default CWNet port)
  uint32_t ptt_tail_ms = 200;          // PTT tail delay for received keying (ms)
};

struct StoredMessagesConfig {
  char message1[128] = "CQ CQ CQ DE";   // F1: CQ message
  char message2[128] = "TEST DE";       // F2: Test message
  char message3[128] = "TU 73";         // F3: Thanks and best regards
  char message4[128] = "QRZ?";          // F4: Who is calling?
  char message5[128] = "";              // F5: Custom message
  char message6[128] = "";              // F6: Custom message
  char message7[128] = "";              // F7: Custom message
  char message8[128] = "";              // F8: Custom message
  char message9[128] = "";              // F9: Custom message
  char message10[128] = "";             // F10: Custom message
};

struct DeviceConfig {
  GeneralConfig general{};       // General device settings (callsign, etc.)
  PaddlePins paddle_pins{};
  OutputPins output_pins{};      // Output GPIO pins (TX, etc.)
  NeoPixelConfig neopixel{};
  I2cPins i2c{};
  I2sPins i2s{};
  CodecConfig codec{};
  IoExpanderConfig io_expander{};
  AudioConfig audio{};
  KeyingConfig keying{};
  WiFiConfig wifi{};
  RemoteConfig remote{};
  ServerConfig server{};
  StoredMessagesConfig stored_messages{};  // Stored CW messages (F1-F8 + keyer speed)
};

// Forward declaration for parameter_table.hpp types
struct ParameterDescriptor;

class Storage {
 public:
  Storage() = default;
  ~Storage();

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  esp_err_t Initialize(const char* nvs_namespace = "keyer");
  DeviceConfig LoadOrDefault();  // Task 3.4: non-const (calls LoadPreviousOrDefaults which may Save)
  esp_err_t Save(const DeviceConfig& config, bool create_backup = true);

  // Apply development defaults from wifi_secrets.h if NVS is empty (Task 5.4.0.8)
  esp_err_t ApplyWiFiSecretsIfEmpty();

  // Task 5.5.1 - Configuration versioning and migration
  // Version 2: Added keying.swap_paddles field
  // Version 3: Added StoredMessagesConfig (10 messages: message1-message10)
  // Version 4: Added remote audio streaming configuration (stream_audio, stream_volume)
  // Version 5: Added per-preset L-S-P timing parameters (preset_definitions[10] array)
  static constexpr uint32_t kCurrentConfigVersion = 5;

  // Task 5.5.2 - Backup/restore/clone functionality
  esp_err_t Backup(const char* backup_namespace, const DeviceConfig* config_to_backup = nullptr);
  esp_err_t Restore(const char* backup_namespace = "keyer_backup");
  esp_err_t Clone(const char* source_namespace, const char* dest_namespace);
  bool HasBackup(const char* backup_namespace = "keyer_backup") const;

 private:
  // Table-driven Load/Save methods (Task 3.3, 3.4, 3.5, 3.8)
  bool LoadParameter(const ParameterDescriptor& desc, DeviceConfig& config) const;
  bool LoadAllParametersFromTable(DeviceConfig& config) const;
  DeviceConfig LoadPreviousOrDefaults();  // Task 3.4: Backup fallback logic (non-const: calls Save)
  DeviceConfig LoadWithoutBackupFallback();  // Load current config without attempting backup restore (prevents recursion in Backup())
  bool SaveParameter(const ParameterDescriptor& desc, const DeviceConfig& config);  // Task 3.5

  // Task 3.8: Legacy Save*Config methods removed - now using table-driven SaveParameter()

  // Migration helpers (Task 5.5.1)
  void MigrateConfig(DeviceConfig& config, uint32_t from_version) const;

  // Task 2.0: Preset customization storage methods
  bool SavePresets(const DeviceConfig& config);                  // Task 2.2
  bool LoadPresets(DeviceConfig& config);                        // Task 2.3
  bool SaveManualLSP(const DeviceConfig& config);                // Task 2.4
  bool LoadManualLSP(DeviceConfig& config);                      // Task 2.5
  void InitializePresetsFromDefaults(DeviceConfig& config);      // Task 2.8

  nvs_handle_t handle_{0};
  bool opened_ = false;
};

}  // namespace config
