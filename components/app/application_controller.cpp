#include "app/application_controller.hpp"
#include "app/usb_early_init.hpp"
#include "app/init_phase.hpp"
#include "app/init_phases.hpp"
#include "app/boot_failure_tracker.hpp"

#include "audio_subsystem/audio_subsystem.hpp"
#include "wifi_subsystem/wifi_subsystem.hpp"
#include "ui/http_server.hpp"
#include "morse_decoder/adaptive_timing_classifier.hpp"
#include "morse_decoder/morse_decoder.hpp"
#include "timeline/timeline_event_emitter.hpp"
#include "text_keyer/text_keyer.hpp"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "hal/high_precision_clock.hpp"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include <cstdarg>
#include <cinttypes>  // For PRIu32
#include <cstdio>
#include <cstring>

namespace app {

namespace {
constexpr char kLogTag[] = "app";

// Task 9.8.3: Main loop delay - balances responsiveness, paddle timing precision, and CPU efficiency
// kMainLoopDelayMs = 1ms → 1000 Hz iteration rate
// CRITICAL: Requires CONFIG_FREERTOS_HZ=1000 in sdkconfig (1 tick = 1ms)
//           If HZ=100, pdMS_TO_TICKS(1) rounds to 0, causing vTaskDelay(0) to yield instead of delay!
// Rationale:
// 1. **Paddle engine memory precision**: At 20 WPM, memory window = 23ms (60%-99% of 60ms dit).
//    1ms polling provides ~23 samples during window for reliable memory arming.
//    Previous 20ms polling provided only 1-2 samples, causing unreliable paddle memory detection.
// 2. **Paddle event latency**: DrainPaddleEvents() processes queued ISR events with <1ms latency.
//    Critical for accurate timing in iambic keying and squeeze detection.
// 3. **LED animation frame rate**: DiagnosticsSubsystem throttles rendering to 50 Hz internally
//    (kMinRenderIntervalUs = 20ms), so increased tick rate doesn't affect LED frame rate.
// 4. **Watchdog coordination**: 500ms watchdog / 1ms loop = 500x safety margin (very safe).
// 5. **CPU impact**: Loop body ~100µs, frequency 1kHz → ~10-15% CPU (acceptable trade-off).
//    vTaskDelay(1ms) yields CPU every millisecond, other tasks remain responsive.
// Trade-off: Higher CPU usage (10-15% vs <1%) but essential for reliable paddle memory timing.
constexpr uint32_t kMainLoopDelayMs = 1;

// Watchdog timeout: Must be >> main loop delay to account for worst-case blocking.
// Main loop: 1ms nominal + worst case: DrainPaddleEvents (burst processing).
// Conservative estimate: 500ms allows 500x main loop iterations before timeout.
constexpr uint32_t kWatchdogTimeoutMs = 500;
}  // namespace

ApplicationController::ApplicationController()
    : config_storage_(),
      device_config_(),
      paddle_hal_(nullptr),  // Will create in Initialize()
      keying_subsystem_(nullptr),  // Will create in Initialize()
      audio_subsystem_(nullptr),  // Will create in Initialize()
      diagnostics_subsystem_(nullptr),  // Will create in Initialize()
      wifi_subsystem_(nullptr),  // Will create in Initialize()
      http_server_(nullptr) {  // Will create in Initialize()
  // Constructor intentionally minimal - all subsystem creation deferred to Initialize()
  // to avoid watchdog timeout and USB conflicts during early boot
}

ApplicationController::~ApplicationController() = default;

[[noreturn]] void ApplicationController::FatalInitError(const char* subsystem,
                                                        esp_err_t error_code) {
  ESP_LOGE(kLogTag, "╔════════════════════════════════════════════════════════════════╗");
  ESP_LOGE(kLogTag, "║              FATAL INITIALIZATION FAILURE                      ║");
  ESP_LOGE(kLogTag, "╠════════════════════════════════════════════════════════════════╣");
  ESP_LOGE(kLogTag, "║ Subsystem: %-50s ║", subsystem);
  ESP_LOGE(kLogTag, "║ Error:     %-50s ║", esp_err_to_name(error_code));
  ESP_LOGE(kLogTag, "║ Code:      0x%04x                                            ║", error_code);
  ESP_LOGE(kLogTag, "╠════════════════════════════════════════════════════════════════╣");
  ESP_LOGE(kLogTag, "║ The device cannot continue with this critical failure.         ║");
  ESP_LOGE(kLogTag, "║ Possible causes:                                               ║");
  ESP_LOGE(kLogTag, "║  - Corrupted NVS flash (try: idf.py erase-flash)              ║");
  ESP_LOGE(kLogTag, "║  - Hardware fault (GPIO, I2C, flash)                          ║");
  ESP_LOGE(kLogTag, "║  - Invalid configuration in storage                            ║");
  ESP_LOGE(kLogTag, "╠════════════════════════════════════════════════════════════════╣");
  ESP_LOGE(kLogTag, "║ System will abort in 2 seconds to trigger coredump...         ║");
  ESP_LOGE(kLogTag, "╚════════════════════════════════════════════════════════════════╝");

  vTaskDelay(pdMS_TO_TICKS(2000));
  ESP_LOGE(kLogTag, "Aborting now. Check coredump for stack trace.");
  abort();
}

hal::PaddleHalConfig ApplicationController::BuildHalConfig(
    const config::DeviceConfig& device_config) {
  hal::PaddleHalConfig hal_config;
  const config::PaddlePins& pins = device_config.paddle_pins;
  const bool active_low = pins.paddles_active_low;
  const bool pull_up = pins.use_pullups;
  const bool pull_down = pins.use_pulldowns;
  const bool swap = device_config.keying.swap_paddles;

  // Apply paddle swap if enabled (useful for left-handed operators or reversed wiring)
  hal_config.dit.gpio = static_cast<gpio_num_t>(swap ? pins.dah_gpio : pins.dit_gpio);
  hal_config.dah.gpio = static_cast<gpio_num_t>(swap ? pins.dit_gpio : pins.dah_gpio);
  hal_config.key.gpio = static_cast<gpio_num_t>(pins.key_gpio);

  hal_config.dit.active_low = active_low;
  hal_config.dah.active_low = active_low;
  hal_config.key.active_low = active_low;

  hal_config.dit.pull_up = pull_up;
  hal_config.dah.pull_up = pull_up;
  hal_config.key.pull_up = pull_up;

  hal_config.dit.pull_down = pull_down;
  hal_config.dah.pull_down = pull_down;
  hal_config.key.pull_down = pull_down;

  return hal_config;
}

void IRAM_ATTR ApplicationController::RecordPaddleEvent(const hal::PaddleEvent& event,
                                                        void* context) {
  auto* controller = static_cast<ApplicationController*>(context);
  if (controller == nullptr) {
    return;
  }

  int64_t timestamp = event.timestamp_us;
  if (timestamp == 0) {
    timestamp = hal::HighPrecisionClock::NowMicros();
  }

  // Diagnostics update moved to KeyingSubsystem::DrainPaddleEvents() (task context)
  // This reduces ISR overhead and eliminates potential spinlock/timing issues

  // Delegate to keying subsystem (enqueue event) - ISR-safe
  keying_subsystem::KeyingSubsystem::RecordPaddleEvent(event, controller->keying_subsystem_.get());
}

bool ApplicationController::Initialize() {
  // Initialize all subsystems using the initialization pipeline
  // Each phase is self-contained and testable, with explicit dependency ordering
  InitializationPipeline pipeline;

  // Phase 0: Bootloader entry check (MUST be first - may jump to factory partition)
  pipeline.AddPhase(std::make_unique<BootloaderCheckPhase>());

  // Phase 0.5: Boot failure tracking (MUST be before NVS init)
  pipeline.AddPhase(std::make_unique<BootFailureCheckPhase>());

  // Phase 1-3: Critical infrastructure (UART, NVS, Clock)
  pipeline.AddPhase(std::make_unique<UartDebugPhase>());
  pipeline.AddPhase(std::make_unique<NvsFlashPhase>());
  pipeline.AddPhase(std::make_unique<HighPrecisionClockPhase>());

  // Phase 4-5: Configuration and parameter registry
  pipeline.AddPhase(std::make_unique<ConfigStoragePhase>(&config_storage_, &device_config_));
  pipeline.AddPhase(std::make_unique<ParameterRegistryPhase>(&param_registry_));

  // Phase 6-7: Diagnostics and USB initialization
  pipeline.AddPhase(std::make_unique<DiagnosticsSubsystemPhase>(&diagnostics_subsystem_, device_config_));
  pipeline.AddPhase(std::make_unique<UsbEarlyInitPhase>(diagnostics_subsystem_.get()));

  // Phase 8: Create subsystem instances
  pipeline.AddPhase(std::make_unique<SubsystemCreationPhase>(this));

  // Phase 9-12: Initialize subsystems
  // NOTE: KeyingSubsystemPhase BEFORE PaddleHalPhase to ensure paddle_event_queue_
  // is created before GPIO interrupts are enabled (prevents race condition)
  pipeline.AddPhase(std::make_unique<KeyingSubsystemPhase>(keying_subsystem_, device_config_));
  pipeline.AddPhase(std::make_unique<TxHalPhase>(tx_hal_, device_config_));
  pipeline.AddPhase(std::make_unique<PaddleHalPhase>(paddle_hal_, device_config_, this));
  pipeline.AddPhase(std::make_unique<AudioSubsystemPhase>(audio_subsystem_, device_config_));

  // Phase 13: Wire subsystem dependencies
  pipeline.AddPhase(std::make_unique<SubsystemWiringPhase>(this));

  // Phase 14-15.5: Network services
  pipeline.AddPhase(std::make_unique<WiFiSubsystemPhase>(wifi_subsystem_, device_config_, diagnostics_subsystem_.get()));
  pipeline.AddPhase(std::make_unique<RemoteClientPhase>(remote_client_, device_config_, keying_subsystem_));
  pipeline.AddPhase(std::make_unique<RemoteServerPhase>(remote_server_, device_config_, tx_hal_.get()));

  // Phase 16-18: Web UI, Captive Portal, and Console
  pipeline.AddPhase(std::make_unique<HttpServerPhase>(http_server_, &device_config_, wifi_subsystem_, &config_storage_, &param_registry_, this));
  pipeline.AddPhase(std::make_unique<CaptivePortalPhase>(captive_portal_manager_, wifi_subsystem_, &device_config_, &config_storage_, http_server_));
  pipeline.AddPhase(std::make_unique<SerialConsolePhase>(&serial_console_, &device_config_, &config_storage_, &param_registry_));

  // Phase 18: Watchdog
  pipeline.AddPhase(std::make_unique<WatchdogPhase>());

  // Execute all phases (handles errors internally)
  bool success = pipeline.Execute();

  // Signal boot complete (Phase 4: Green LED)
  if (diagnostics_subsystem_ && diagnostics_subsystem_->IsReady()) {
    diagnostics_subsystem_->SignalBootPhase(4);  // Green: Boot complete
  }

  // Clear boot failure counter on successful initialization
  // This prevents entering safe mode on next boot if this boot succeeds
  ClearBootFailureCount();

  ESP_LOGI(kLogTag, "Initialization complete");
  return success;
}


esp_err_t ApplicationController::ConfigureWatchdog() {
  // Initialize task watchdog timer with our custom configuration
  // Note: With CONFIG_ESP_TASK_WDT_INIT=y, watchdog is already initialized
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = kWatchdogTimeoutMs,
      .idle_core_mask = 0,  // Don't monitor IDLE tasks (we only care about main loop)
      .trigger_panic = true  // Trigger panic on timeout for coredump/debugging
  };

  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    // Watchdog already initialized by CONFIG_ESP_TASK_WDT_INIT=y - this is OK
    ESP_LOGI(kLogTag, "Watchdog already initialized by system (CONFIG_ESP_TASK_WDT_INIT=y)");
  } else if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to initialize watchdog: %s", esp_err_to_name(err));
    return err;
  }

  // Subscribe current task (main task running app_main) to watchdog
  err = esp_task_wdt_add(xTaskGetCurrentTaskHandle());
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to add task to watchdog: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(kLogTag, "Watchdog configured: timeout=%lums, panic_on_timeout=true",
           static_cast<unsigned long>(kWatchdogTimeoutMs));
  return ESP_OK;
}


void ApplicationController::Run() {
  ESP_LOGI(kLogTag, "Entering main loop");

  ESP_LOGI(kLogTag, "PROFILING MODE ENABLED - Logging subsystem timing every 5 seconds");

  // Profiling variables
  static int64_t last_profile_log_us = 0;
  static uint32_t loop_count = 0;
  static int64_t total_drain_us = 0;
  static int64_t total_keying_us = 0;
  static int64_t total_wifi_us = 0;
  static int64_t total_remote_server_us = 0;
  static int64_t total_diagnostics_us = 0;
  static int64_t total_loop_us = 0;

  while (true) {
    const int64_t loop_start_us = esp_timer_get_time();

#ifdef PADDLE_USE_POLLING
    // Polling mode: Read GPIO pins and generate events for detected edges
    if (paddle_hal_) {
      paddle_hal_->Poll();
    }
#endif

    // Drain paddle events and update keying engine
    const int64_t t0 = esp_timer_get_time();
    keying_subsystem_->DrainPaddleEvents();
    const int64_t t1 = esp_timer_get_time();

    const int64_t now_us = hal::HighPrecisionClock::NowMicros();
    keying_subsystem_->Tick(now_us);
    const int64_t t2 = esp_timer_get_time();

    // Monitor WiFi connection state (Task 5.4.0.7)
    const int64_t t3 = esp_timer_get_time();
    if (wifi_subsystem_) {
      const uint32_t now_ms = static_cast<uint32_t>(now_us / 1000);
      wifi_subsystem_->Tick(now_ms);

      // Dynamically enable/disable captive portal based on WiFi mode and client count
      // Throttled to check only every 500ms to reduce CPU usage
      if (captive_portal_manager_) {
        static uint32_t last_captive_check_ms = 0;
        if (now_ms - last_captive_check_ms >= 500) {  // Check every 500ms (not every 1ms)
          last_captive_check_ms = now_ms;

          const wifi_subsystem::WiFiStatus wifi_status = wifi_subsystem_->GetStatus();
          const uint8_t client_count = wifi_subsystem_->GetConnectedClientCount();

          // Enable captive portal if in AP mode with at least one client connected
          const bool should_be_active = (wifi_status.mode == wifi_subsystem::WiFiMode::kApActive) && (client_count > 0);

          if (should_be_active && !captive_portal_manager_->IsActive()) {
            ESP_LOGI(kLogTag, "Enabling captive portal (AP mode + %u client(s) connected)", client_count);
            captive_portal_manager_->Enable();
            // Start rainbow pattern LED animation to indicate setup mode
            if (diagnostics_subsystem_) {
              diagnostics_subsystem_->SetRainbowPattern();
            }
          } else if (!should_be_active && captive_portal_manager_->IsActive()) {
            ESP_LOGI(kLogTag, "Disabling captive portal (AP mode inactive or no clients)");
            captive_portal_manager_->Disable();
            // Stop rainbow pattern and return to normal LED status
            if (diagnostics_subsystem_) {
              diagnostics_subsystem_->StopRainbowPattern();
            }
          }
        }
      }
    }
    const int64_t t4 = esp_timer_get_time();

    // Remote CW client now runs in dedicated FreeRTOS task (no Tick needed)
    // See: docs/plans/2025-11-15-remotecw-task-architecture-design.md

    // Tick remote CW server (accept connections, RX/TX I/O, PTT management)
#ifdef CONFIG_ENABLE_MAIN_LOOP_PROFILING
    const int64_t t5 = esp_timer_get_time();
#endif
    if (remote_server_) {
      remote_server_->Tick(now_us);
    }
#ifdef CONFIG_ENABLE_MAIN_LOOP_PROFILING
    const int64_t t6 = esp_timer_get_time();
#endif

    // Tick text keyer (keyboard morse code sending state machine)
    if (text_keyer_) {
      text_keyer_->Tick(now_us);
    }

    // Update diagnostics (LED animations)
#ifdef CONFIG_ENABLE_MAIN_LOOP_PROFILING
    const int64_t t7 = esp_timer_get_time();
#endif
    if (diagnostics_subsystem_) {
      diagnostics_subsystem_->Tick();
    }
#ifdef CONFIG_ENABLE_MAIN_LOOP_PROFILING
    const int64_t t8 = esp_timer_get_time();

    const int64_t loop_end_us = esp_timer_get_time();

    // Accumulate timing statistics
    total_drain_us += (t1 - t0);
    total_keying_us += (t2 - t1);
    total_wifi_us += (t4 - t3);
    total_remote_server_us += (t6 - t5);
    total_diagnostics_us += (t8 - t7);
    total_loop_us += (loop_end_us - loop_start_us);
    loop_count++;

    // Log profiling statistics every 5 seconds
    if (last_profile_log_us == 0 || (loop_end_us - last_profile_log_us) >= 5000000) {
      if (loop_count > 0) {
        const int64_t avg_drain = total_drain_us / loop_count;
        const int64_t avg_keying = total_keying_us / loop_count;
        const int64_t avg_wifi = total_wifi_us / loop_count;
        const int64_t avg_server = total_remote_server_us / loop_count;
        const int64_t avg_diagnostics = total_diagnostics_us / loop_count;
        const int64_t avg_loop = total_loop_us / loop_count;

        ESP_LOGI(kLogTag, "╔════════════════════════════════════════════════════════════════╗");
        ESP_LOGI(kLogTag, "║         MAIN LOOP PROFILING (%lu iterations)                   ", loop_count);
        ESP_LOGI(kLogTag, "╠════════════════════════════════════════════════════════════════╣");
        ESP_LOGI(kLogTag, "║ DrainPaddleEvents:  %6lld µs  (%5.1f%%)                         ",
                 avg_drain, (avg_loop > 0 ? 100.0 * avg_drain / avg_loop : 0.0));
        ESP_LOGI(kLogTag, "║ Keying Tick:        %6lld µs  (%5.1f%%)                         ",
                 avg_keying, (avg_loop > 0 ? 100.0 * avg_keying / avg_loop : 0.0));
        ESP_LOGI(kLogTag, "║ WiFi Tick:          %6lld µs  (%5.1f%%)                         ",
                 avg_wifi, (avg_loop > 0 ? 100.0 * avg_wifi / avg_loop : 0.0));
        ESP_LOGI(kLogTag, "║ Remote Server Tick: %6lld µs  (%5.1f%%)                         ",
                 avg_server, (avg_loop > 0 ? 100.0 * avg_server / avg_loop : 0.0));
        ESP_LOGI(kLogTag, "║ Diagnostics Tick:   %6lld µs  (%5.1f%%)                         ",
                 avg_diagnostics, (avg_loop > 0 ? 100.0 * avg_diagnostics / avg_loop : 0.0));
        ESP_LOGI(kLogTag, "╠════════════════════════════════════════════════════════════════╣");
        ESP_LOGI(kLogTag, "║ TOTAL LOOP TIME:    %6lld µs / 1000 µs (%.1f%% busy)           ",
                 avg_loop, (avg_loop / 10.0));
        ESP_LOGI(kLogTag, "╠════════════════════════════════════════════════════════════════╣");
        ESP_LOGI(kLogTag, "║ Expected: ~100 µs/loop (10%% CPU)                              ║");
        ESP_LOGI(kLogTag, "║ If >500 µs/loop: Performance issue detected!                  ║");
        ESP_LOGI(kLogTag, "╚════════════════════════════════════════════════════════════════╝");
      }

      // Reset statistics
      last_profile_log_us = loop_end_us;
      loop_count = 0;
      total_drain_us = 0;
      total_keying_us = 0;
      total_wifi_us = 0;
      total_remote_server_us = 0;
      total_diagnostics_us = 0;
      total_loop_us = 0;
    }
#endif  // CONFIG_ENABLE_MAIN_LOOP_PROFILING

    // NOTE: Watchdog monitoring delegated to system IDLE tasks (CONFIG_ESP_TASK_WDT_INIT=y)
    // Main task no longer registered to avoid false triggers when serial_console busy

    vTaskDelay(pdMS_TO_TICKS(kMainLoopDelayMs));
  }
}

void ApplicationController::ApplyConfigChanges(const config::DeviceConfig& new_config) {
  ESP_LOGI("app", "Applying configuration changes to running subsystems");

  // Apply to keying subsystem (speed, preset, memory windows)
  if (keying_subsystem_) {
    keying_subsystem_->ApplyConfig(new_config);
  }

  // Apply to audio subsystem (frequency, volume, fade)
  if (audio_subsystem_) {
    audio_subsystem_->ApplyConfig(new_config);
  }

  // Apply to diagnostics subsystem (future: LED settings)
  if (diagnostics_subsystem_) {
    diagnostics_subsystem_->ApplyConfig(new_config);
  }

  // Apply decoder enabled state (can be toggled at runtime)
  if (morse_decoder_) {
    morse_decoder_->SetEnabled(new_config.keying.decoder_enabled);
    ESP_LOGI("app", "Decoder %s from config update",
             new_config.keying.decoder_enabled ? "enabled" : "disabled");
  }

  // Update internal config reference
  device_config_ = new_config;

  ESP_LOGI("app", "Configuration changes applied successfully");
}

}  // namespace app
