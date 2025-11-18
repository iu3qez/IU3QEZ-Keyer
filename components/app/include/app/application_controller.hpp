#pragma once

/**
 * @file application_controller.hpp
 * @brief Application Controller - Thin orchestrator for firmware lifecycle
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * This is a **thin orchestrator** that delegates to functional subsystems.
 * After refactoring from monolithic main.cpp, this layer was further decomposed
 * into domain-specific subsystems to improve readability and maintainability.
 *
 * DESIGN PRINCIPLES:
 * - **Separation of concerns**: Each subsystem owns its domain logic
 * - **Dependency injection**: Subsystems get references to config/storage/other subsystems
 * - **Explicit initialization order**: Initialize() calls subsystems in dependency order
 * - **Fail-fast on critical failures**: FatalInitError() for unrecoverable errors
 * - **Main loop delegation**: Run() delegates to subsystem Tick() methods
 *
 * RESPONSIBILITIES (kept minimal):
 * - Initialize NVS flash and high-precision clock (ESP-IDF primitives)
 * - Load/manage device configuration (config::Storage)
 * - Initialize PaddleHal and register ISR callbacks
 * - Create subsystem instances and wire dependencies
 * - Orchestrate main loop (drain paddle events, tick subsystems)
 * - Handle fatal errors with diagnostic logging
 */

#include "config/device_config.hpp"
#include "config/parameter_registry.hpp"
#include "diagnostics_subsystem/diagnostics_subsystem.hpp"
#include "hal/paddle_hal.hpp"
#include "hal/tx_hal.hpp"
#include "keying_subsystem/keying_subsystem.hpp"
#include <memory>

namespace audio_subsystem {
class AudioSubsystem;
}  // namespace audio_subsystem

namespace wifi_subsystem {
class WiFiSubsystem;
}  // namespace wifi_subsystem

namespace ui {
class HttpServer;
class SerialConsole;
}  // namespace ui

namespace remote {
class RemoteCwClient;
class RemoteCwServer;
}  // namespace remote

namespace timeline {
class TimelineEventEmitter;
}  // namespace timeline

namespace text_keyer {
class TextKeyer;
}  // namespace text_keyer

namespace captive_portal {
class CaptivePortalManager;
}  // namespace captive_portal

namespace app {

/**
 * @brief Thin application orchestrator that delegates to functional subsystems.
 *
 * Coordinates initialization and runtime lifecycle for:
 * - KeyingSubsystem (paddle events, engine, timeline)
 * - AudioSubsystem (sidetone service)
 * - DiagnosticsSubsystem (status LED, activity visualization)
 * - WiFiSubsystem (STA/AP networking)
 * - HttpServer (Web UI for configuration)
 */
class ApplicationController {
 public:
  ApplicationController();
  ~ApplicationController();

  // Non-copyable, non-movable (owns hardware resources).
  ApplicationController(const ApplicationController&) = delete;
  ApplicationController& operator=(const ApplicationController&) = delete;

  /**
   * @brief Initialize all subsystems in dependency order.
   *
   * Initialization sequence:
   * 1. NVS flash (persistent storage backend)
   * 2. High-precision clock (microsecond timestamps)
   * 3. Configuration storage + load device config
   * 4. Paddle HAL (GPIO interrupts, ISR registration)
   * 5. KeyingSubsystem (paddle engine, timeline, event queue)
   * 6. AudioSubsystem (sidetone service, I2C/I2S/codec)
   * 7. DiagnosticsSubsystem (status LED, activity visualization)
   * 8. WiFiSubsystem (STA/AP networking)
   * 9. HttpServer (Web UI on port 80)
   * 10. Watchdog configuration
   *
   * CRITICAL failures (config, HAL, keying) trigger FatalInitError() and abort.
   * NON-CRITICAL failures (audio, diagnostics, wifi, http, watchdog) log warnings and continue.
   *
   * @return true if all critical subsystems initialized successfully.
   */
  bool Initialize();

  /**
   * @brief Main application loop - delegates to subsystems.
   *
   * Loop sequence:
   * 1. Drain paddle events (KeyingSubsystem)
   * 2. Tick keying engine (KeyingSubsystem)
   * 3. Tick WiFi connection monitor (WiFiSubsystem)
   * 4. Update LED animations (DiagnosticsSubsystem)
   * 5. Reset watchdog timer
   * 6. Delay 20ms (50 Hz main loop rate)
   */
  [[noreturn]] void Run();

  /**
   * @brief Apply configuration changes to running subsystems (hot-reload).
   *
   * Propagates runtime-changeable parameters to all subsystems without requiring
   * device reset. Only affects parameters that can be safely changed at runtime:
   * - Keying: speed_wpm, preset, memory windows, late release
   * - Audio: sidetone frequency, volume, fade in/out
   * - Diagnostics: (future) LED settings
   *
   * Hardware parameters (GPIO pins, I2C/I2S config) require device reset.
   *
   * @param new_config Updated device configuration
   */
  void ApplyConfigChanges(const config::DeviceConfig& new_config);

  /**
   * @brief Get remote CW client instance (for Web UI status).
   * @return Pointer to RemoteCwClient, or nullptr if not initialized.
   */
  remote::RemoteCwClient* GetRemoteClient() { return remote_client_.get(); }

  /**
   * @brief Get remote CW server instance (for Web UI status).
   * @return Pointer to RemoteCwServer, or nullptr if not initialized.
   */
  remote::RemoteCwServer* GetRemoteServer() { return remote_server_.get(); }

  /**
   * @brief Get morse decoder instance (for Web UI status).
   * @return Pointer to MorseDecoder, or nullptr if not initialized.
   */
  morse_decoder::MorseDecoder* GetMorseDecoder() const {
    if (keying_subsystem_) {
      return keying_subsystem_->GetMorseDecoder();
    }
    return nullptr;
  }

  /**
   * @brief Get keying subsystem instance (for timeline API, Web UI).
   * @return Pointer to KeyingSubsystem, or nullptr if not initialized.
   */
  keying_subsystem::KeyingSubsystem* GetKeyingSubsystem() const {
    return keying_subsystem_.get();
  }

  /**
   * @brief Get timeline event emitter instance (for diagnostics, testing).
   * @return Pointer to TimelineEventEmitter, or nullptr if not initialized.
   */
  timeline::TimelineEventEmitter* GetTimelineEmitter() const {
    return timeline_emitter_.get();
  }

  /**
   * @brief Get text keyer instance (for Web UI control).
   * @return Pointer to TextKeyer, or nullptr if not initialized.
   */
  text_keyer::TextKeyer* GetTextKeyer() const {
    return text_keyer_.get();
  }

  /**
   * @brief Fatal initialization error handler: log banner and abort().
   *
   * Public to allow use by InitializationPipeline and initialization phases.
   * Static method - can be called before ApplicationController fully constructed.
   *
   * @param subsystem Human-readable subsystem name (e.g., "NVS Flash", "Paddle HAL")
   * @param error_code ESP-IDF error code from failed initialization
   */
  [[noreturn]] static void FatalInitError(const char* subsystem, esp_err_t error_code);

 private:
  // Friend declarations for initialization phases that need access to private members
  friend class SubsystemCreationPhase;
  friend class SubsystemWiringPhase;
  friend class PaddleHalPhase;
  friend class RemoteClientPhase;
  friend class RemoteServerPhase;

  /**
   * @brief Build HAL configuration from device config (GPIO mappings).
   */
  static hal::PaddleHalConfig BuildHalConfig(const config::DeviceConfig& device_config);

  /**
   * @brief Combined ISR callback: updates diagnostics + enqueues event to keying subsystem.
   */
  static void IRAM_ATTR RecordPaddleEvent(const hal::PaddleEvent& event, void* context);

  /**
   * @brief Configure task watchdog for main loop monitoring (Task 9.4).
   * @return ESP_OK on success, error code on failure.
   */
  esp_err_t ConfigureWatchdog();

  // Core configuration and storage
  config::Storage config_storage_;
  config::DeviceConfig device_config_;
  config::ParameterRegistry param_registry_;  // Parameter metadata system (injected into console)

  // Hardware abstraction layer
  std::unique_ptr<hal::PaddleHal> paddle_hal_;
  std::unique_ptr<hal::TxHal> tx_hal_;  // TX key output HAL

  // Functional subsystems
  std::unique_ptr<keying_subsystem::KeyingSubsystem> keying_subsystem_;
  std::unique_ptr<audio_subsystem::AudioSubsystem> audio_subsystem_;
  std::unique_ptr<diagnostics_subsystem::DiagnosticsSubsystem> diagnostics_subsystem_;
  std::unique_ptr<wifi_subsystem::WiFiSubsystem> wifi_subsystem_;
  std::unique_ptr<remote::RemoteCwClient> remote_client_;
  std::unique_ptr<remote::RemoteCwServer> remote_server_;
  std::unique_ptr<ui::HttpServer> http_server_;
  std::unique_ptr<ui::SerialConsole> serial_console_;

  // Morse decoder subsystem (Phase 2 - integrated with KeyingSubsystem)
  std::unique_ptr<morse_decoder::AdaptiveTimingClassifier> timing_classifier_;
  std::unique_ptr<morse_decoder::MorseDecoder> morse_decoder_;

  // Timeline event emitter (real-time visualization and diagnostics)
  std::unique_ptr<timeline::TimelineEventEmitter> timeline_emitter_;

  // Text keyer (keyboard morse code sending)
  std::unique_ptr<text_keyer::TextKeyer> text_keyer_;

  // Captive portal manager (WiFi setup in AP mode)
  std::unique_ptr<captive_portal::CaptivePortalManager> captive_portal_manager_;
};

}  // namespace app
