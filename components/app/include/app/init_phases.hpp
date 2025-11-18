#pragma once

/**
 * @file init_phases.hpp
 * @brief Concrete initialization phase classes for ApplicationController
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * Each class implements one discrete step in the boot sequence, extracted from
 * the monolithic ApplicationController::Initialize() method (originally 199 LOC).
 *
 * PHASE OVERVIEW (18 phases, execution order):
 * ==============================================
 * 0.  BootloaderCheckPhase  - Check RTC memory for bootloader entry flag (CRITICAL)
 * 0.5 BootFailureCheckPhase - Track boot failures, enter safe mode if bootloop (CRITICAL)
 * 1.  UartDebugPhase        - UART1 debug output + log hook (CRITICAL)
 * 2.  NvsFlashPhase         - Non-volatile storage backend (CRITICAL)
 * 3.  HighPrecisionClockPhase - Microsecond timestamps via esp_timer (CRITICAL)
 * 4.  ConfigStoragePhase    - Load device config from NVS (CRITICAL)
 * 5.  ParameterRegistryPhase - Populate parameter metadata registry
 * 6.  DiagnosticsSubsystemPhase - LED driver + boot phase signals 0-1
 * 7.  UsbEarlyInitPhase     - TinyUSB CDC console init
 * 8.  SubsystemCreationPhase - Instantiate paddle_hal, tx_hal, keying, audio, wifi, http (CRITICAL)
 * 9.  KeyingSubsystemPhase  - Paddle engine + timeline (CRITICAL)
 * 10. TxHalPhase            - TX key output GPIO
 * 11. PaddleHalPhase        - GPIO interrupts for paddle inputs (CRITICAL)
 * 12. AudioSubsystemPhase   - Sidetone service (codec + I2S)
 * 13. SubsystemWiringPhase  - Wire dependencies + boot phase signal 2 (CRITICAL)
 * 14. WiFiSubsystemPhase    - WiFi STA/AP networking + boot phase signal 3
 * 15. HttpServerPhase       - Web UI on port 80
 * 16. WatchdogPhase         - Task watchdog timer
 *
 * HARDWARE DEPENDENCIES (must respect ordering):
 * ===============================================
 * ESP32-S3 hardware initialization requires strict ordering to avoid crashes
 * and undefined behavior. Violating these dependencies may cause:
 * - Null pointer dereferences (subsystems accessed before creation)
 * - GPIO conflicts (multiple drivers claiming same pin)
 * - NVS corruption (config operations before flash init)
 * - Missing log output (ESP_LOGI before UART/USB ready)
 * - Hard faults (peripheral access before clock/power ready)
 *
 * CRITICAL DEPENDENCIES:
 * - BootloaderCheckPhase BEFORE any other phase (must check RTC memory first)
 *   └─ Reason: If bootloader flag set, device must jump to factory partition immediately
 * - BootFailureCheckPhase AFTER BootloaderCheckPhase, BEFORE NvsFlashPhase
 *   └─ Reason: Manual bootloader entry takes precedence; bootloop detection before NVS access
 * - UartDebugPhase BEFORE any ESP_LOGI calls
 *   └─ Reason: Log hook must be installed before any subsystem logs
 * - NvsFlashPhase BEFORE ConfigStoragePhase (config stored in NVS)
 *   └─ Reason: nvs_get_* operations require NVS partition initialized
 * - HighPrecisionClockPhase BEFORE any esp_timer_* usage
 *   └─ Reason: Timeline and diagnostics require microsecond timestamps
 * - ConfigStoragePhase BEFORE subsystem creation (subsystems need config)
 *   └─ Reason: GPIO pin assignments, I2S config, WiFi credentials from DeviceConfig
 * - DiagnosticsSubsystemPhase BEFORE UsbEarlyInitPhase (LED white signal)
 *   └─ Reason: USB init triggers LED visual feedback (requires LED driver ready)
 * - SubsystemCreationPhase BEFORE subsystem init phases (instances must exist)
 *   └─ Reason: paddle_hal_, tx_hal_, keying_subsystem_ must exist before Init() called
 * - KeyingSubsystemPhase BEFORE PaddleHalPhase (queue must exist before ISR enabled)
 *   └─ Reason: PaddleHal ISR writes to paddle_event_queue_ (must be created first)
 * - AudioSubsystemPhase BEFORE SubsystemWiringPhase (audio must be ready)
 *   └─ Reason: Wiring phase calls SetAudioSubsystem() on keying (requires audio initialized)
 * - SubsystemWiringPhase AFTER all subsystem init (wires dependencies)
 *   └─ Reason: SetTxHal(), SetAudioSubsystem() require fully initialized subsystems
 * - WiFiSubsystemPhase BEFORE HttpServerPhase (server needs network)
 *   └─ Reason: HTTP server binds to WiFi IP address (requires WiFi connected)
 *
 * EXAMPLE FAILURE SCENARIO (if ordering violated):
 * If PaddleHalPhase runs before KeyingSubsystemPhase:
 *   1. PaddleHal::Init() enables GPIO interrupts and registers ISR callback
 *   2. Paddle input causes GPIO interrupt before KeyingSubsystem creates queue
 *   3. ISR calls RecordPaddleEvent() with paddle_event_queue_ == nullptr
 *   4. Result: Events dropped silently during boot (race condition)
 *
 * LED BOOT PHASE SIGNALS:
 * =======================
 * - Phase 0 (Magenta): After NVS init (DiagnosticsSubsystemPhase)
 * - Phase 1 (Cyan): After config loaded (DiagnosticsSubsystemPhase)
 * - Phase 2 (Orange): After subsystem wiring (SubsystemWiringPhase)
 * - Phase 3 (Yellow): Before WiFi init (WiFiSubsystemPhase)
 * - Phase 4 (Green): After boot complete (ApplicationController::Initialize)
 */

#include "app/init_phase.hpp"
#include "app/application_controller.hpp"
#include "config/device_config.hpp"
#include "config/parameter_registry.hpp"
#include "diagnostics_subsystem/diagnostics_subsystem.hpp"
#include "hal/paddle_hal.hpp"
#include "hal/tx_hal.hpp"
#include "keying_subsystem/keying_subsystem.hpp"
#include "audio_subsystem/audio_subsystem.hpp"
#include "wifi_subsystem/wifi_subsystem.hpp"
#include "remote/remote_cw_client.hpp"
#include "remote/remote_cw_server.hpp"
#include "ui/http_server.hpp"
#include "ui/serial_console.hpp"
#include "captive_portal/captive_portal_manager.hpp"

namespace app {

// Forward declaration for subsystem creation phase
class ApplicationController;

//=============================================================================
// Phase 0: Bootloader Entry Check (CRITICAL)
//=============================================================================
/**
 * @brief Check RTC memory for bootloader entry flag before any other initialization.
 *
 * CRITICAL: Must run FIRST to intercept bootloader entry requests.
 * RTC MEMORY: Survives soft reset but not power cycle (perfect for reboot-triggered entry).
 * JUMP TARGET: Factory partition at 0x34000 (1MB tinyuf2 bootloader).
 *
 * If bootloader flag detected:
 *   1. Clears RTC memory flag (single-use)
 *   2. Jumps to factory partition
 *   3. Never returns
 *
 * If no flag:
 *   Boot sequence continues normally
 */
class BootloaderCheckPhase : public InitPhase {
 public:
  esp_err_t Execute() override;
  const char* GetName() const override { return "Bootloader Check"; }
  bool IsCritical() const override { return true; }
};

//=============================================================================
// Phase 0.5: Boot Failure Tracking (CRITICAL)
//=============================================================================
/**
 * @brief Track consecutive boot failures and enter safe mode if bootloop detected
 *
 * Increments RTC-based failure counter on every boot. If threshold reached
 * (3 consecutive failures), enters safe mode by jumping to bootloader.
 *
 * MUST run after Phase 0 (BootloaderCheckPhase) to avoid interfering with
 * manual bootloader entry. MUST run before Phase 2 (NvsFlashPhase) to detect
 * NVS corruption issues before attempting NVS access.
 *
 * On bootloop detection:
 * - Logs error message with recovery instructions
 * - Enters UF2 bootloader mode
 * - User can delete FACTORY_RESET.TXT to erase NVS
 * - Device restarts with factory defaults
 */
class BootFailureCheckPhase : public InitPhase {
 public:
  esp_err_t Execute() override;
  const char* GetName() const override { return "Boot Failure Check"; }
  bool IsCritical() const override { return true; }
};

//=============================================================================
// Phase 1: UART Debug Output (CRITICAL)
//=============================================================================
/**
 * @brief Initialize UART1 on GPIO6 for debug output before USB CDC available.
 *
 * CRITICAL: Must run before any ESP_LOGI calls (installs log hook).
 * HARDWARE: GPIO6 TX @ 115200 baud, no RX needed (output only).
 * LOG HOOK: Static lambda persists after phase completes (converts LF → CRLF).
 */
class UartDebugPhase : public InitPhase {
 public:
  esp_err_t Execute() override;
  const char* GetName() const override { return "UART Debug"; }
  bool IsCritical() const override { return true; }
};

//=============================================================================
// Phase 2: NVS Flash (CRITICAL)
//=============================================================================
/**
 * @brief Initialize ESP-IDF NVS flash backend (erase on corruption).
 *
 * CRITICAL: Required for config storage, WiFi credentials, partition table.
 * ERROR RECOVERY: Erases NVS if corrupted (ESP_ERR_NVS_NO_FREE_PAGES).
 */
class NvsFlashPhase : public InitPhase {
 public:
  esp_err_t Execute() override;
  const char* GetName() const override { return "NVS Flash"; }
  bool IsCritical() const override { return true; }
};

//=============================================================================
// Phase 3: High-Precision Clock (CRITICAL)
//=============================================================================
/**
 * @brief Enable esp_timer for microsecond timestamps (keying timing).
 *
 * CRITICAL: Required for paddle timing, audio sample clocking, timeline.
 * DEPENDS ON: Nothing (pure ESP-IDF primitive).
 */
class HighPrecisionClockPhase : public InitPhase {
 public:
  esp_err_t Execute() override;
  const char* GetName() const override { return "High-Precision Clock"; }
  bool IsCritical() const override { return true; }
};

//=============================================================================
// Phase 4: Configuration Storage (CRITICAL)
//=============================================================================
/**
 * @brief Initialize config storage, apply WiFi secrets, load DeviceConfig.
 *
 * CRITICAL: Required for all subsystems (GPIO pins, audio settings, etc.).
 * DEPENDS ON: NvsFlashPhase (config stored in NVS).
 * WIFI SECRETS: Applies development credentials on first boot (Task 5.4.0.8).
 */
class ConfigStoragePhase : public InitPhase {
 public:
  ConfigStoragePhase(config::Storage* storage, config::DeviceConfig* device_config)
      : storage_(storage), device_config_(device_config) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Configuration Storage"; }
  bool IsCritical() const override { return true; }

 private:
  config::Storage* storage_;
  config::DeviceConfig* device_config_;
};

//=============================================================================
// Phase 5: Parameter Registry
//=============================================================================
/**
 * @brief Populate parameter metadata registry for console/Web UI.
 *
 * NON-CRITICAL: Always succeeds (logs warning if registry empty).
 * DEPENDS ON: Nothing (pure metadata registration).
 */
class ParameterRegistryPhase : public InitPhase {
 public:
  explicit ParameterRegistryPhase(config::ParameterRegistry* registry)
      : registry_(registry) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Parameter Registry"; }
  bool IsCritical() const override { return false; }

 private:
  config::ParameterRegistry* registry_;
};

//=============================================================================
// Phase 6: Diagnostics Subsystem (LED signals)
//=============================================================================
/**
 * @brief Create and initialize DiagnosticsSubsystem, signal boot phases 0-1.
 *
 * NON-CRITICAL: Logs warning on failure (no LED feedback, firmware works).
 * LED SIGNALS: Phase 0 (Magenta: NVS), Phase 1 (Cyan: Config loaded).
 * DEPENDS ON: ConfigStoragePhase (needs neopixel GPIO from config).
 */
class DiagnosticsSubsystemPhase : public InitPhase {
 public:
  DiagnosticsSubsystemPhase(
      std::unique_ptr<diagnostics_subsystem::DiagnosticsSubsystem>* subsystem,
      const config::DeviceConfig& config)
      : subsystem_(subsystem), config_(config) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Diagnostics Subsystem"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<diagnostics_subsystem::DiagnosticsSubsystem>* subsystem_;
  const config::DeviceConfig& config_;
};

//=============================================================================
// Phase 7: USB Early Init
//=============================================================================
/**
 * @brief Signal USB init starting (LED white), initialize TinyUSB CDC.
 *
 * NON-CRITICAL: Logs warning on failure (UART debug still available).
 * LED SIGNAL: White LEDs for 1 second (gives user time to open COM port).
 * DEPENDS ON: DiagnosticsSubsystemPhase (for LED signal).
 */
class UsbEarlyInitPhase : public InitPhase {
 public:
  explicit UsbEarlyInitPhase(diagnostics_subsystem::DiagnosticsSubsystem* diagnostics)
      : diagnostics_(diagnostics) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "USB Early Init"; }
  bool IsCritical() const override { return false; }

 private:
  diagnostics_subsystem::DiagnosticsSubsystem* diagnostics_;
};

//=============================================================================
// Phase 8: Subsystem Creation (CRITICAL)
//=============================================================================
/**
 * @brief Create subsystem instances (paddle_hal, tx_hal, keying, audio, wifi, http).
 *
 * CRITICAL: Memory allocation failures are fatal.
 * DEPENDS ON: Nothing (pure object instantiation).
 */
class SubsystemCreationPhase : public InitPhase {
 public:
  explicit SubsystemCreationPhase(ApplicationController* controller)
      : controller_(controller) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Subsystem Creation"; }
  bool IsCritical() const override { return true; }

 private:
  ApplicationController* controller_;
};

//=============================================================================
// Phase 9: Paddle HAL (CRITICAL)
//=============================================================================
/**
 * @brief Initialize paddle GPIO interrupts + register ISR callback.
 *
 * CRITICAL: Required for keying (no paddle input = no functionality).
 * DEPENDS ON: SubsystemCreationPhase (paddle_hal instance), ConfigStoragePhase (GPIO pins).
 */
class PaddleHalPhase : public InitPhase {
 public:
  PaddleHalPhase(std::unique_ptr<hal::PaddleHal>& paddle_hal,
                 const config::DeviceConfig& config,
                 void* callback_context)
      : paddle_hal_(paddle_hal), config_(config), callback_context_(callback_context) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Paddle HAL"; }
  bool IsCritical() const override { return true; }

 private:
  std::unique_ptr<hal::PaddleHal>& paddle_hal_;
  const config::DeviceConfig& config_;
  void* callback_context_;
};

//=============================================================================
// Phase 10: TX HAL
//=============================================================================
/**
 * @brief Initialize TX key output GPIO (PTT/CW keying line).
 *
 * NON-CRITICAL: Logs warning on failure (keying works, no TX output).
 * DEPENDS ON: SubsystemCreationPhase (tx_hal instance), ConfigStoragePhase (GPIO pin).
 */
class TxHalPhase : public InitPhase {
 public:
  TxHalPhase(std::unique_ptr<hal::TxHal>& tx_hal, const config::DeviceConfig& config)
      : tx_hal_(tx_hal), config_(config) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "TX HAL"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<hal::TxHal>& tx_hal_;
  const config::DeviceConfig& config_;
};

//=============================================================================
// Phase 11: Keying Subsystem (CRITICAL)
//=============================================================================
/**
 * @brief Initialize keying subsystem (paddle engine + timeline).
 *
 * CRITICAL: Core functionality (iambic keying, memory, timing).
 * DEPENDS ON: SubsystemCreationPhase (keying instance), PaddleHalPhase (paddle input).
 */
class KeyingSubsystemPhase : public InitPhase {
 public:
  KeyingSubsystemPhase(std::unique_ptr<keying_subsystem::KeyingSubsystem>& subsystem,
                       const config::DeviceConfig& config)
      : subsystem_(subsystem), config_(config) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Keying Subsystem"; }
  bool IsCritical() const override { return true; }

 private:
  std::unique_ptr<keying_subsystem::KeyingSubsystem>& subsystem_;
  const config::DeviceConfig& config_;
};

//=============================================================================
// Phase 12: Audio Subsystem
//=============================================================================
/**
 * @brief Initialize audio subsystem (sidetone service, codec, I2S).
 *
 * NON-CRITICAL: Logs warning on failure (silent mode, keying still works).
 * NOTE: Does NOT call Start() - controlled by keying state changes.
 * DEPENDS ON: SubsystemCreationPhase (audio instance), ConfigStoragePhase (I2C/I2S pins).
 */
class AudioSubsystemPhase : public InitPhase {
 public:
  AudioSubsystemPhase(std::unique_ptr<audio_subsystem::AudioSubsystem>& subsystem,
                      const config::DeviceConfig& config)
      : subsystem_(subsystem), config_(config) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Audio Subsystem"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<audio_subsystem::AudioSubsystem>& subsystem_;
  const config::DeviceConfig& config_;
};

//=============================================================================
// Phase 13: Subsystem Wiring (CRITICAL)
//=============================================================================
/**
 * @brief Wire subsystem dependencies (keying→tx, keying→audio, diagnostics→timeline).
 *
 * CRITICAL: Wiring failures indicate logic errors.
 * LED SIGNAL: Phase 2 (Orange: Subsystems ready).
 * DEPENDS ON: All subsystem init phases (instances must be initialized).
 */
class SubsystemWiringPhase : public InitPhase {
 public:
  explicit SubsystemWiringPhase(ApplicationController* controller)
      : controller_(controller) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Subsystem Wiring"; }
  bool IsCritical() const override { return true; }

 private:
  ApplicationController* controller_;
};

//=============================================================================
// Phase 14: WiFi Subsystem
//=============================================================================
/**
 * @brief Initialize WiFi subsystem (STA/AP networking).
 *
 * NON-CRITICAL: Logs warning on failure (offline mode, local keying works).
 * LED SIGNAL: Phase 3 (Yellow: WiFi init starting).
 * DEPENDS ON: ConfigStoragePhase (SSID/password), DiagnosticsSubsystemPhase (LED animation).
 */
class WiFiSubsystemPhase : public InitPhase {
 public:
  WiFiSubsystemPhase(std::unique_ptr<wifi_subsystem::WiFiSubsystem>& subsystem,
                     const config::DeviceConfig& config,
                     diagnostics_subsystem::DiagnosticsSubsystem* diagnostics)
      : subsystem_(subsystem), config_(config), diagnostics_(diagnostics) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "WiFi Subsystem"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<wifi_subsystem::WiFiSubsystem>& subsystem_;
  const config::DeviceConfig& config_;
  diagnostics_subsystem::DiagnosticsSubsystem* diagnostics_;
};

//=============================================================================
// Phase 15: Remote CW Client
//=============================================================================
/**
 * @brief Initialize CWNet remote keying client (optional, enabled via config).
 *
 * NON-CRITICAL: Logs warning on failure (local keying still works).
 * CONDITIONAL: Only starts if remote.enabled=true and WiFi initialized.
 * DEPENDS ON: WiFiSubsystemPhase (needs network), SubsystemWiringPhase (needs KeyingSubsystem wired).
 */
class RemoteClientPhase : public InitPhase {
 public:
  RemoteClientPhase(std::unique_ptr<remote::RemoteCwClient>& client,
                    const config::DeviceConfig& config,
                    std::unique_ptr<keying_subsystem::KeyingSubsystem>& keying)
      : client_(client), config_(config), keying_(keying) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Remote CW Client"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<remote::RemoteCwClient>& client_;
  const config::DeviceConfig& config_;
  std::unique_ptr<keying_subsystem::KeyingSubsystem>& keying_;
};

//=============================================================================
// Phase 15.5: Remote CW Server
//=============================================================================
/**
 * @brief Initialize CWNet server for receiving remote keying (optional, enabled via config).
 *
 * NON-CRITICAL: Logs warning on failure (local keying still works).
 * CONDITIONAL: Only starts if server.enabled=true and WiFi initialized.
 * DEPENDS ON: WiFiSubsystemPhase (needs network), SubsystemWiringPhase (needs TX HAL wired).
 */
class RemoteServerPhase : public InitPhase {
 public:
  RemoteServerPhase(std::unique_ptr<remote::RemoteCwServer>& server,
                    const config::DeviceConfig& config,
                    hal::TxHal* tx_hal)
      : server_(server), config_(config), tx_hal_(tx_hal) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Remote CW Server"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<remote::RemoteCwServer>& server_;
  const config::DeviceConfig& config_;
  hal::TxHal* tx_hal_;
};

//=============================================================================
// Phase 16: HTTP Server
//=============================================================================
/**
 * @brief Initialize HTTP server (Web UI on port 80).
 *
 * NON-CRITICAL: Logs warning on failure (console still available).
 * CONDITIONAL: Only starts if WiFi initialized successfully.
 * DEPENDS ON: WiFiSubsystemPhase (needs IP address).
 */
class HttpServerPhase : public InitPhase {
 public:
  HttpServerPhase(std::unique_ptr<ui::HttpServer>& server,
                  config::DeviceConfig* device_config,
                  std::unique_ptr<wifi_subsystem::WiFiSubsystem>& wifi,
                  config::Storage* storage,
                  config::ParameterRegistry* registry,
                  ApplicationController* app_controller)
      : server_(server), device_config_(device_config), wifi_(wifi),
        storage_(storage), registry_(registry), app_controller_(app_controller) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "HTTP Server"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<ui::HttpServer>& server_;
  config::DeviceConfig* device_config_;
  std::unique_ptr<wifi_subsystem::WiFiSubsystem>& wifi_;
  config::Storage* storage_;
  config::ParameterRegistry* registry_;
  ApplicationController* app_controller_;
};

//=============================================================================
// Phase 15.5: Captive Portal Manager
//=============================================================================
/**
 * @brief Initialize captive portal manager for AP mode WiFi setup.
 *
 * NON-CRITICAL: Logs warning on failure (can still configure via main HTTP server if WiFi connected).
 * FEATURES: DNS wildcard redirect, minimal HTTP server, WiFi scan and configuration.
 * DEPENDS ON: WiFiSubsystemPhase (WiFi available), HttpServerPhase (setup page needs to coexist).
 *
 * The captive portal is dynamically enabled/disabled based on WiFi mode and client count.
 * When AP mode is active with at least one client, the captive portal starts automatically.
 */
class CaptivePortalPhase : public InitPhase {
 public:
  CaptivePortalPhase(std::unique_ptr<captive_portal::CaptivePortalManager>& manager,
                     std::unique_ptr<wifi_subsystem::WiFiSubsystem>& wifi,
                     config::DeviceConfig* device_config,
                     config::Storage* storage,
                     std::unique_ptr<ui::HttpServer>& main_http_server)
      : manager_(manager), wifi_(wifi), device_config_(device_config), storage_(storage),
        main_http_server_(main_http_server) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Captive Portal"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<captive_portal::CaptivePortalManager>& manager_;
  std::unique_ptr<wifi_subsystem::WiFiSubsystem>& wifi_;
  config::DeviceConfig* device_config_;
  config::Storage* storage_;
  std::unique_ptr<ui::HttpServer>& main_http_server_;
};

//=============================================================================
// Phase 16: Serial Console
//=============================================================================
/**
 * @brief Initialize serial console on USB-CDC1 (COM7) for runtime parameter configuration.
 *
 * NON-CRITICAL: Logs warning on failure (HTTP server still available for configuration).
 * FEATURES: Command-line interface with TAB completion, history, line editing.
 * DEPENDS ON: UsbEarlyInitPhase (USB-CDC initialized), ParameterRegistryPhase (parameters available).
 */
class SerialConsolePhase : public InitPhase {
 public:
  SerialConsolePhase(std::unique_ptr<ui::SerialConsole>* console,
                     config::DeviceConfig* device_config,
                     config::Storage* storage,
                     config::ParameterRegistry* registry)
      : console_(console), device_config_(device_config), storage_(storage), registry_(registry) {}

  esp_err_t Execute() override;
  const char* GetName() const override { return "Serial Console"; }
  bool IsCritical() const override { return false; }

 private:
  std::unique_ptr<ui::SerialConsole>* console_;
  config::DeviceConfig* device_config_;
  config::Storage* storage_;
  config::ParameterRegistry* registry_;
};

//=============================================================================
// Phase 17: Watchdog
//=============================================================================
/**
 * @brief Configure task watchdog timer for main loop monitoring.
 *
 * NON-CRITICAL: Logs warning on failure (no watchdog protection).
 * DEPENDS ON: Nothing (pure ESP-IDF configuration).
 */
class WatchdogPhase : public InitPhase {
 public:
  esp_err_t Execute() override;
  const char* GetName() const override { return "Watchdog"; }
  bool IsCritical() const override { return false; }
};

}  // namespace app
