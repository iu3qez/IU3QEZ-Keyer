/**
 * @file init_phases.cpp
 * @brief Implementation of 17 concrete initialization phases
 *
 * Each Execute() method is extracted from ApplicationController::Initialize(),
 * preserving the exact logic, error handling, and LED signaling behavior.
 *
 * See init_phases.hpp for architecture rationale and dependency ordering.
 */

#include "app/init_phases.hpp"
#include "app/bootloader_entry.hpp"
#include "app/boot_failure_tracker.hpp"

// ESP-IDF includes
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

// Project includes
#include "config/device_config.hpp"
#include "config/parameter_registry.hpp"
#include "hal/high_precision_clock.hpp"
#include "ui/console_parameter_bridge.hpp"
#include "ui/console_system_commands.hpp"
#include "morse_decoder/adaptive_timing_classifier.hpp"
#include "morse_decoder/morse_decoder.hpp"
#include "timeline/timeline_event_emitter.hpp"
#include "text_keyer/text_keyer.hpp"

// Forward declare usb_early_init() from usb_early_init.cpp
extern "C" esp_err_t usb_early_init();

// Forward declare RegisterAllParameters() from parameter_registry.cpp
namespace config {
extern void RegisterAllParameters(ParameterRegistry& registry);
}

namespace app {

namespace {
constexpr const char* kLogTag = "init_phases";
constexpr uint32_t kWatchdogTimeoutMs = 20000;  // 20 seconds (from application_controller.cpp)
}

//=============================================================================
// Phase 0: Bootloader Entry Check
//=============================================================================
esp_err_t BootloaderCheckPhase::Execute() {
  // Check RTC memory for bootloader entry flag
  // This must run BEFORE any other initialization (including UART/logs)
  // because we may need to jump to factory partition immediately

  if (CheckBootloaderFlag()) {
    // Flag detected - jump to factory partition (tinyuf2 bootloader)
    const esp_partition_t* factory_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);

    if (factory_partition == nullptr) {
      // Factory partition not found - this is a fatal error
      // Can't use ESP_LOGE yet (UART not initialized)
      // Fall through and continue normal boot (will fail later with proper logging)
      return ESP_FAIL;
    }

    // Set factory partition as next boot partition
    esp_err_t err = esp_ota_set_boot_partition(factory_partition);
    if (err != ESP_OK) {
      return err;
    }

    // Restart to boot into factory partition (tinyuf2 bootloader)
    esp_restart();

    // Never returns
    while (true) {}
  }

  // No bootloader flag - continue normal boot
  return ESP_OK;
}

//=============================================================================
// Phase 0.5: Boot Failure Check
//=============================================================================

esp_err_t BootFailureCheckPhase::Execute() {
  // Increment boot failure counter (RTC memory persists across soft resets)
  uint8_t count = IncrementBootFailureCount();

  // Check if bootloop detected (threshold reached)
  if (IsBootloopDetected()) {
    // CRITICAL: Bootloop detected - enter safe mode
    // At this point, we can't log yet (UART not initialized), so we rely on
    // the log messages in IncrementBootFailureCount() after UART init in next phase

    // Enter bootloader mode for recovery
    // User can delete FACTORY_RESET.TXT from bootloader drive to reset NVS
    EnterBootloaderMode();

    // Never returns (device restarts into bootloader)
    while (true) {}
  }

  // Boot failure count incremented but below threshold - continue boot
  // Counter will be cleared if boot succeeds (in final init phase)
  return ESP_OK;
}

//=============================================================================
// Phase 1: UART Debug Output
//=============================================================================
esp_err_t UartDebugPhase::Execute() {
  // Initialize UART1 on GPIO6 for debug output (before anything else)
  uart_config_t uart_cfg = {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  uart_driver_install(UART_NUM_1, 256, 0, 0, nullptr, 0);
  uart_param_config(UART_NUM_1, &uart_cfg);
  uart_set_pin(UART_NUM_1, 6, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

  const char* banner = "\r\n\r\n=== ESP32-S3 UART1 Debug (GPIO6 TX @ 115200) ===\r\n";
  uart_write_bytes(UART_NUM_1, banner, strlen(banner));

  // Install temporary UART1 log hook BEFORE any ESP_LOGI calls
  // This captures early boot logs (NVS, config, WiFi secrets) before USB CDC is initialized
  // When usb_early_init() installs its hook later, it will chain to this one
  // Note: Static buffers are safe here - ESP-IDF logging is serialized
  static auto uart1_log_hook = [](const char* fmt, va_list args) -> int {
    static char buffer[128];      // Reduced size to avoid stack overflow
    static char crlf_buffer[256]; // Double size for worst case (all \n → \r\n)
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (len > 0) {
      // Convert LF to CRLF for proper serial terminal display
      size_t src_len = (len < sizeof(buffer)) ? len : sizeof(buffer) - 1;
      size_t dst_idx = 0;
      for (size_t i = 0; i < src_len && dst_idx < sizeof(crlf_buffer) - 1; i++) {
        if (buffer[i] == '\n') {
          crlf_buffer[dst_idx++] = '\r';  // Add CR before LF
        }
        crlf_buffer[dst_idx++] = buffer[i];
      }
      uart_write_bytes(UART_NUM_1, crlf_buffer, dst_idx);
    }
    return len;
  };
  esp_log_set_vprintf(+uart1_log_hook);

  ESP_LOGI(kLogTag, "UART1 debug output initialized");

  // Configure default log level based on boot failure state
  // - Normal boot: Only ERROR/WARN messages (reduce serial noise)
  // - Bootloop detected: Enable DEBUG logs (for diagnostics)
  // User can still override via 'debug' console command at runtime
  if (IsBootloopDetected()) {
    // Bootloop detected - enable debug logs for diagnostics
    esp_log_level_set("*", ESP_LOG_DEBUG);
    ESP_LOGW(kLogTag, "BOOTLOOP DETECTED - Debug logs enabled for diagnostics");
  } else {
    // Normal boot - suppress debug/info logs (only warnings and errors)
    esp_log_level_set("*", ESP_LOG_WARN);
    // Use WARN level since we just set log level to WARN (INFO would be suppressed)
    ESP_LOGW(kLogTag, "Log level set to WARN (use 'debug' command to enable verbose logging)");
  }

  return ESP_OK;
}

//=============================================================================
// Phase 2: NVS Flash
//=============================================================================
esp_err_t NvsFlashPhase::Execute() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kLogTag, "NVS corrupted, erasing...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  return err;
}

//=============================================================================
// Phase 3: High-Precision Clock
//=============================================================================
esp_err_t HighPrecisionClockPhase::Execute() {
  return hal::HighPrecisionClock::Initialize();
}

//=============================================================================
// Phase 4: Configuration Storage
//=============================================================================
esp_err_t ConfigStoragePhase::Execute() {
  esp_err_t err = storage_->Initialize();
  if (err != ESP_OK) {
    return err;
  }

  // Apply WiFi secrets from wifi_secrets.h if NVS is empty (Task 5.4.0.8)
  // This writes development credentials to NVS on first boot for convenience
  err = storage_->ApplyWiFiSecretsIfEmpty();
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Failed to apply WiFi secrets: %s (continuing)", esp_err_to_name(err));
  }

  *device_config_ = storage_->LoadOrDefault();
  return ESP_OK;
}

//=============================================================================
// Phase 5: Parameter Registry
//=============================================================================
esp_err_t ParameterRegistryPhase::Execute() {
  // Populate registry with all parameters (audio, keying, general)
  // This happens BEFORE subsystem initialization so registry is available to all consumers
  // (console, future Web UI, API, etc.) regardless of their initialization order
  config::RegisterAllParameters(*registry_);
  ESP_LOGI(kLogTag, "Parameter registry initialized with %zu parameters",
           registry_->GetParameterCount());
  return ESP_OK;
}

//=============================================================================
// Phase 6: Diagnostics Subsystem
//=============================================================================
esp_err_t DiagnosticsSubsystemPhase::Execute() {
  // Create DiagnosticsSubsystem early for LED visual signal before USB CDC init
  *subsystem_ = std::make_unique<diagnostics_subsystem::DiagnosticsSubsystem>();
  esp_err_t err = (*subsystem_)->Initialize(config_);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Diagnostics subsystem init failed (continuing without LED)");
    return err;
  }

  // Signal boot phases with LED colors
  if (*subsystem_ && (*subsystem_)->IsReady()) {
    (*subsystem_)->SignalBootPhase(0);  // Magenta: NVS
    (*subsystem_)->SignalBootPhase(1);  // Cyan: Config loaded
  }

  return ESP_OK;
}

//=============================================================================
// Phase 7: USB Early Init
//=============================================================================
esp_err_t UsbEarlyInitPhase::Execute() {
  // Visual signal: LED turns white for 1 second
  if (diagnostics_) {
    diagnostics_->SignalUsbInitStarting();
  }

  // Early USB initialization (after LED signal)
  esp_err_t err = usb_early_init();
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Early USB init failed: %s (continuing)", esp_err_to_name(err));
  }
  return err;
}

//=============================================================================
// Phase 8: Subsystem Creation
//=============================================================================
esp_err_t SubsystemCreationPhase::Execute() {
  // Create remaining subsystem instances (deferred from constructor to avoid early boot issues)
  // Access controller's private members via friend declaration
  controller_->paddle_hal_ = std::make_unique<hal::PaddleHal>();
  controller_->tx_hal_ = std::make_unique<hal::TxHal>();
  controller_->keying_subsystem_ = std::make_unique<keying_subsystem::KeyingSubsystem>();
  controller_->audio_subsystem_ = std::make_unique<audio_subsystem::AudioSubsystem>();
  controller_->wifi_subsystem_ = std::make_unique<wifi_subsystem::WiFiSubsystem>();
  controller_->http_server_ = std::make_unique<ui::HttpServer>();

  // Create remote keying subsystems (always instantiate for runtime control via console/web UI)
  controller_->remote_client_ = std::make_unique<remote::RemoteCwClient>();
  controller_->remote_server_ = std::make_unique<remote::RemoteCwServer>();

  // Create morse decoder subsystem (Phase 2 - adaptive timing + decoding)
  controller_->timing_classifier_ = std::make_unique<morse_decoder::AdaptiveTimingClassifier>(
      25.0f);  // 25% timing tolerance (from config)

  morse_decoder::MorseDecoderConfig decoder_config{};
  decoder_config.buffer_size = 100;              // Default buffer size
  decoder_config.enable_logging = false;         // Disable verbose logging by default
  decoder_config.timing_classifier = controller_->timing_classifier_.get();

  controller_->morse_decoder_ = std::make_unique<morse_decoder::MorseDecoder>(decoder_config);

  // Create TimelineEventEmitter for real-time visualization
  auto& timeline = controller_->keying_subsystem_->GetTimeline();
  controller_->timeline_emitter_ = std::make_unique<timeline::TimelineEventEmitter>(&timeline);

  // Wire TimelineEventEmitter to KeyingSubsystem BEFORE Initialize() is called
  // (paddle_engine needs hooks ready during initialization)
  controller_->keying_subsystem_->SetTimelineEmitter(controller_->timeline_emitter_.get());
  ESP_LOGI(kLogTag, "Timeline event emitter created and wired (hooks ready for paddle engine init)");

  // Create text keyer for keyboard morse code sending
  controller_->text_keyer_ = std::make_unique<text_keyer::TextKeyer>();
  ESP_LOGI(kLogTag, "Text keyer created");

  ESP_LOGI(kLogTag, "Subsystem instances created (including remote client/server, morse decoder, timeline emitter, and text keyer)");
  return ESP_OK;
}

//=============================================================================
// Phase 9: Paddle HAL
//=============================================================================
esp_err_t PaddleHalPhase::Execute() {
  ESP_LOGI(kLogTag, "Building paddle HAL config...");
  // Build HAL config from device config (extracted from ApplicationController::BuildHalConfig)
  hal::PaddleHalConfig hal_config{};
  const config::PaddlePins& pins = config_.paddle_pins;
  const bool swap = config_.keying.swap_paddles;

  ESP_LOGI(kLogTag, "Paddle GPIOs: dit=%d, dah=%d, key=%d (swap=%s)",
           static_cast<int>(pins.dit_gpio),
           static_cast<int>(pins.dah_gpio),
           static_cast<int>(pins.key_gpio),
           swap ? "YES" : "NO");

  // Apply paddle swap if enabled (useful for left-handed operators or reversed wiring)
  hal_config.dit.gpio = static_cast<gpio_num_t>(swap ? pins.dah_gpio : pins.dit_gpio);
  hal_config.dah.gpio = static_cast<gpio_num_t>(swap ? pins.dit_gpio : pins.dah_gpio);
  hal_config.key.gpio = static_cast<gpio_num_t>(pins.key_gpio);

  hal_config.dit.active_low = pins.paddles_active_low;
  hal_config.dah.active_low = pins.paddles_active_low;
  hal_config.key.active_low = pins.paddles_active_low;

  hal_config.dit.pull_up = pins.use_pullups;
  hal_config.dah.pull_up = pins.use_pullups;
  hal_config.key.pull_up = pins.use_pullups;

  hal_config.dit.pull_down = pins.use_pulldowns;
  hal_config.dah.pull_down = pins.use_pulldowns;
  hal_config.key.pull_down = pins.use_pulldowns;

  ESP_LOGI(kLogTag, "Calling paddle_hal_->Initialize()...");
  // Initialize paddle HAL with callback
  // Use ApplicationController::RecordPaddleEvent static method as callback
  esp_err_t err = paddle_hal_->Initialize(hal_config, ApplicationController::RecordPaddleEvent, callback_context_);
  ESP_LOGI(kLogTag, "paddle_hal_->Initialize() returned: %s", esp_err_to_name(err));
  if (err != ESP_OK) {
    return err;
  }

  if (!paddle_hal_->HasConfiguredPins()) {
    ESP_LOGW(kLogTag, "Paddle pins not configured; update NVS to enable sensing");
  }

  return ESP_OK;
}

//=============================================================================
// Phase 10: TX HAL
//=============================================================================
esp_err_t TxHalPhase::Execute() {
  const gpio_num_t trx_gpio = static_cast<gpio_num_t>(config_.output_pins.trx_gpio);
  const bool trx_active_high = config_.output_pins.trx_active_high;
  esp_err_t err = tx_hal_->Initialize(trx_gpio, trx_active_high);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "TX HAL init failed: %s (continuing without TX output)", esp_err_to_name(err));
  } else if (trx_gpio != GPIO_NUM_NC) {
    ESP_LOGI(kLogTag, "TX output ready (GPIO=%d, polarity=%s)",
             static_cast<int>(trx_gpio), trx_active_high ? "active_high" : "active_low");
  }
  return err;
}

//=============================================================================
// Phase 11: Keying Subsystem
//=============================================================================
esp_err_t KeyingSubsystemPhase::Execute() {
  esp_err_t err = subsystem_->Initialize(config_);
  if (err == ESP_OK) {
    // Register keying subsystem with console debug commands
    ui::SetKeyingSubsystem(subsystem_.get());
  }
  return err;
}

//=============================================================================
// Phase 12: Audio Subsystem
//=============================================================================
esp_err_t AudioSubsystemPhase::Execute() {
  esp_err_t err = subsystem_->Initialize(config_);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Audio subsystem init failed (continuing without audio)");
  }
  // Note: Do NOT call audio_subsystem_->Start() here!
  // The sidetone will be controlled by HandleKeyingStateChanged() in sync with TX
  return err;
}

//=============================================================================
// Phase 13: Subsystem Wiring
//=============================================================================
esp_err_t SubsystemWiringPhase::Execute() {
  // Wire KeyingSubsystem with TxHal, AudioSubsystem, and DiagnosticsSubsystem
  controller_->keying_subsystem_->SetTxHal(controller_->tx_hal_.get());
  controller_->keying_subsystem_->SetAudioSubsystem(controller_->audio_subsystem_.get());
  controller_->keying_subsystem_->SetDiagnosticsSubsystem(controller_->diagnostics_subsystem_.get());

  // Wire morse decoder subsystem into KeyingSubsystem (Phase 2)
  controller_->keying_subsystem_->SetTimingClassifier(controller_->timing_classifier_.get());
  controller_->keying_subsystem_->SetMorseDecoder(controller_->morse_decoder_.get());

  // Register decoder with console commands (enables "decoder" command)
  if (controller_->morse_decoder_) {
    ui::SetMorseDecoder(controller_->morse_decoder_.get());
  }

  // Apply decoder enabled state from configuration
  if (controller_->morse_decoder_) {
    controller_->morse_decoder_->SetEnabled(controller_->device_config_.keying.decoder_enabled);
    ESP_LOGI(kLogTag, "Decoder %s from configuration",
             controller_->device_config_.keying.decoder_enabled ? "enabled" : "disabled");
  }

  // Wire TimelineLogger into MorseDecoder for gap marker emission
  // (Note: TimelineEventEmitter already wired in SubsystemCreationPhase before engine init)
  if (controller_->morse_decoder_) {
    controller_->morse_decoder_->SetTimelineLogger(&controller_->keying_subsystem_->GetTimeline());
    ESP_LOGI(kLogTag, "Timeline logger wired to MorseDecoder for gap markers");
  }

  ESP_LOGI(kLogTag, "KeyingSubsystem wired with TX HAL, Audio, Diagnostics, Morse Decoder, and Timeline");

  // Wire AudioStreamPlayer from AudioSubsystem into RemoteCwClient for remote RX audio
  if (controller_->audio_subsystem_ && controller_->audio_subsystem_->IsReady() && controller_->remote_client_) {
    auto* stream_player = controller_->audio_subsystem_->GetStreamPlayer();
    if (stream_player != nullptr) {
      controller_->remote_client_->SetAudioStreamPlayer(stream_player);
      ESP_LOGI(kLogTag, "AudioStreamPlayer injected into RemoteCwClient for remote audio streaming");
    } else {
      ESP_LOGW(kLogTag, "AudioStreamPlayer unavailable - remote audio RX disabled");
    }
  }

  // Wire DiagnosticsSubsystem with KeyingSubsystem timeline
  if (controller_->diagnostics_subsystem_ && controller_->diagnostics_subsystem_->IsReady()) {
    controller_->diagnostics_subsystem_->SetTimelineLogger(&controller_->keying_subsystem_->GetTimeline());
    controller_->diagnostics_subsystem_->SignalBootPhase(2);  // Orange: Subsystems ready
  }

  // Initialize text keyer for keyboard morse code sending
  if (controller_->text_keyer_) {
    esp_err_t err = controller_->text_keyer_->Initialize(
        controller_->tx_hal_.get(),
        controller_->audio_subsystem_.get());
    if (err == ESP_OK) {
      // Set initial speed from global keying config
      controller_->text_keyer_->SetSpeed(controller_->device_config_.keying.speed_wpm);
      ESP_LOGI(kLogTag, "Text keyer initialized (speed=%u WPM)",
               controller_->device_config_.keying.speed_wpm);
    } else {
      ESP_LOGW(kLogTag, "Text keyer initialization failed: %s", esp_err_to_name(err));
    }
  }

  return ESP_OK;
}

//=============================================================================
// Phase 14: WiFi Subsystem
//=============================================================================
esp_err_t WiFiSubsystemPhase::Execute() {
  if (diagnostics_ && diagnostics_->IsReady()) {
    diagnostics_->SignalBootPhase(3);  // Yellow: WiFi init starting
  }
  // Pass diagnostics subsystem pointer for WiFi LED animation (Task 1.7)
  esp_err_t err = subsystem_->Initialize(config_.wifi, diagnostics_);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "WiFi subsystem init failed: %s (continuing without WiFi)", esp_err_to_name(err));
  }
  return err;
}

//=============================================================================
// Phase 15: Remote CW Client
//=============================================================================
esp_err_t RemoteClientPhase::Execute() {
  // RemoteCwClient instance already created in SubsystemCreationPhase
  ESP_LOGI(kLogTag, "RemoteClientPhase: keying_=%p, client_=%p", keying_.get(), client_.get());
  // Register with console commands (always, even if disabled, for status/control)
  ui::SetRemoteClient(client_.get());
  // Check if remote client is enabled in config
  if (!config_.remote.enabled) {
    ESP_LOGI(kLogTag, "Remote CW client disabled (remote.enabled=false) - instance available for runtime activation");
  }

  // Configure client (always, even if server_host empty - allows runtime start via Web UI)
  ESP_LOGI(kLogTag, "Configuring Remote CW client...");

  remote::RemoteCwClientConfig client_config{};
  client_config.server_host = config_.remote.server_host;
  client_config.server_port = config_.remote.server_port;
  client_config.callsign = config_.general.callsign;  // Reuse general callsign
  client_config.stream_audio = config_.remote.stream_audio;
  client_config.stream_volume = config_.remote.stream_volume;

  // Configure callbacks (minimal for now, will be enhanced later with console/Web UI integration)
  remote::RemoteCwClientCallbacks callbacks{};
  callbacks.on_state_changed = nullptr;      // TODO: Add state change handler for Web UI
  callbacks.on_remote_key_event = nullptr;   // Not used in client-only mode
  callbacks.on_latency_updated = nullptr;    // TODO: Add latency handler for Web UI
  callbacks.on_print_message = nullptr;      // TODO: Add print message handler for console
  callbacks.context = nullptr;

  ESP_EARLY_LOGW(kLogTag, "ABOUT TO CALL client_->Configure() - client_=%p", client_.get());
  client_->Configure(client_config, callbacks);
  ESP_EARLY_LOGW(kLogTag, "RETURNED FROM client_->Configure()");

  // Wire remote client to keying subsystem for PTT management
  if (keying_) {
    keying_->SetRemoteClient(client_.get(), config_.remote.ptt_tail_ms);
    ESP_LOGI(kLogTag, "Remote CW client wired to KeyingSubsystem (PTT tail: %lu ms)",
             static_cast<unsigned long>(config_.remote.ptt_tail_ms));
  } else {
    ESP_LOGE(kLogTag, "CRITICAL BUG: keying_ unique_ptr is empty! Remote client cannot be wired to KeyingSubsystem!");
  }

  // Check if server host is configured for auto-start
  if (config_.remote.server_host[0] == '\0') {
    ESP_LOGW(kLogTag, "Remote CW client configured but server_host not set - start manually via Web UI/console");
    return ESP_OK;  // Not critical - can be started later
  }

  // Start connection if auto_reconnect is enabled
  if (config_.remote.auto_reconnect) {
    esp_err_t err = client_->Start();
    if (err != ESP_OK) {
      ESP_LOGW(kLogTag, "Remote CW client start failed: %s (will retry automatically)",
               esp_err_to_name(err));
    } else {
      ESP_LOGI(kLogTag, "Remote CW client started (connecting to %s:%u)",
               config_.remote.server_host, config_.remote.server_port);
    }
  }

  return ESP_OK;
}

//=============================================================================
// Phase 15.5: Remote CW Server
//=============================================================================
esp_err_t RemoteServerPhase::Execute() {
  // RemoteCwServer instance already created in SubsystemCreationPhase
  // Register with console commands (always, even if disabled, for status/control)
  ui::SetRemoteServer(server_.get());
  // Check if server is enabled in config
  if (!config_.server.enabled) {
    ESP_LOGI(kLogTag, "Remote CW server disabled (server.enabled=false) - instance available for runtime activation");
    return ESP_OK;  // Not an error - feature is optional
  }

  // Configure server
  ESP_LOGI(kLogTag, "Configuring Remote CW server...");
  // Configure server
  remote::RemoteCwServerConfig server_config{};
  server_config.listen_port = config_.server.listen_port;
  server_config.auto_restart = true;  // Always auto-restart server on error
  server_config.ptt_tail_ms = config_.server.ptt_tail_ms;

  // Configure callbacks for received keying
  remote::RemoteCwServerCallbacks callbacks{};
  callbacks.on_state_changed = nullptr;  // TODO: Add state change handler for Web UI

  // Wire server received keying → TX HAL output
  if (tx_hal_) {
    callbacks.on_key_event = [](bool key_down, int64_t timestamp_us, void* context) {
      auto* tx_hal = static_cast<hal::TxHal*>(context);
      if (tx_hal) {
        tx_hal->SetActive(key_down);
      }
    };
    callbacks.context = tx_hal_;
    ESP_LOGI(kLogTag, "Remote CW server wired to TX HAL for keying output");
  } else {
    ESP_LOGW(kLogTag, "TX HAL not available, server keying will not drive output");
  }

  server_->Configure(server_config, callbacks);

  // Start listening
  esp_err_t err = server_->Start();
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Remote CW server start failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(kLogTag, "Remote CW server listening on port %u", config_.server.listen_port);
  }


  return ESP_OK;
}

//=============================================================================
// Phase 16: HTTP Server
//=============================================================================
esp_err_t HttpServerPhase::Execute() {
  // Only start HTTP server if WiFi is ready
  if (!wifi_->IsInitialized()) {
    ESP_LOGI(kLogTag, "Skipping HTTP server (WiFi not initialized)");
    return ESP_OK;  // Not an error - expected when WiFi disabled
  }

  esp_err_t err = server_->Initialize(device_config_, wifi_.get(), storage_, registry_, app_controller_);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "HTTP server init failed: %s (continuing without Web UI)", esp_err_to_name(err));
  } else {
    ESP_LOGI(kLogTag, "Web UI available at http://%s/",
             wifi_->GetStatus().ip_address);
  }
  return err;
}

//=============================================================================
// Phase 15.5: Captive Portal Manager
//=============================================================================
esp_err_t CaptivePortalPhase::Execute() {
  // Only create captive portal if WiFi is initialized
  if (!wifi_->IsInitialized()) {
    ESP_LOGI(kLogTag, "Skipping captive portal (WiFi not initialized)");
    return ESP_OK;  // Not an error - expected when WiFi disabled
  }

  ESP_LOGI(kLogTag, "Creating captive portal manager...");

  // Create captive portal manager instance (with main HTTP server reference for port 80 coordination)
  manager_ = std::make_unique<captive_portal::CaptivePortalManager>(
      wifi_.get(), device_config_, storage_, nullptr, main_http_server_.get());

  // Initialize (creates DNS server and minimal HTTP server instances)
  if (!manager_->Initialize()) {
    ESP_LOGW(kLogTag, "Captive portal init failed (continuing without captive portal)");
    return ESP_FAIL;
  }

  ESP_LOGI(kLogTag, "Captive portal initialized (will activate when AP mode + clients connected)");
  return ESP_OK;
}

//=============================================================================
// Phase 16: Serial Console
//=============================================================================
esp_err_t SerialConsolePhase::Execute() {
  static constexpr char kLogTag[] = "init_phases";

  ESP_LOGI(kLogTag, "Initializing serial console on USB-CDC1 (COM7)...");

  // Create console instance with DeviceConfig for dynamic prompt
  *console_ = std::make_unique<ui::SerialConsole>(device_config_);

  // Set global console instance pointer for command handlers
  ui::g_console_instance = console_->get();

  // Register parameter management commands BEFORE starting the task (Task 3.0)
  // This prevents race condition where task tries to process commands before they're registered
  ui::RegisterParameterCommands(console_->get(), registry_, device_config_, storage_);

  // Register system commands (debug, reboot, factory-reset) - Task 4.0
  ui::RegisterSystemCommands(console_->get());

  // Initialize console and start FreeRTOS task (AFTER commands are registered)
  (*console_)->Init();

  ESP_LOGI(kLogTag, "Serial console initialized successfully");
  return ESP_OK;
}

//=============================================================================
// Phase 17: Watchdog
//=============================================================================
esp_err_t WatchdogPhase::Execute() {
  // Initialize task watchdog timer with our custom configuration
  // Note: With CONFIG_ESP_TASK_WDT_INIT=y, watchdog is already initialized
  static constexpr uint32_t kWatchdogTimeoutMs = 5000;  // 5 seconds timeout
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = kWatchdogTimeoutMs,
      .idle_core_mask = 0,  // Don't monitor IDLE tasks (we only care about main loop)
      .trigger_panic = true  // Trigger panic on timeout for coredump/debugging
  };

  // NOTE: Watchdog is auto-initialized by CONFIG_ESP_TASK_WDT_INIT=y with IDLE task monitoring.
  // We do NOT register the main task because serial_console can monopolize CPU causing false triggers.
  // System IDLE task watchdog is sufficient to detect true hangs.

  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    ESP_LOGI(kLogTag, "Watchdog already initialized by system (CONFIG_ESP_TASK_WDT_INIT=y) - using system config");
    return ESP_OK;
  } else if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to initialize watchdog: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(kLogTag, "Watchdog configured (timeout=%lums)", static_cast<unsigned long>(kWatchdogTimeoutMs));
  return ESP_OK;
}

}  // namespace app
