/**
 * @file serial_console.cpp
 * @brief Serial Console implementation for USB-CDC interface
 *
 * Main implementation of the SerialConsole class.
 * Adapted from prototype in components/ui/tmp/Console.cpp
 * Modified to use USB-CDC (TINYUSB_CDC_ACM_1) instead of UART.
 */

#include "ui/serial_console.hpp"
#include "InputHandler.hpp"
#include "CommandDispatcher.hpp"
#include "OutputBuffer.hpp"
#include "config/device_config.hpp"
#include "tinyusb_cdc_acm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"  // For esp_app_get_description()
#include <sstream>
#include <cstring>
#include <cstdarg>

namespace ui {

static const char* TAG = "SerialConsole";

// Global console instance pointer (used by command handlers in console_parameter_bridge.cpp)
SerialConsole* g_console_instance = nullptr;

// USB-CDC port for console (COM7) - already initialized in usb_early_init.cpp
static constexpr tinyusb_cdcacm_itf_t CONSOLE_CDC_PORT = TINYUSB_CDC_ACM_1;

// FreeRTOS task entry point
static void console_task_entry(void* pvParameters) {
    auto* console = static_cast<SerialConsole*>(pvParameters);
    ESP_LOGI(TAG, "Console task started");

    while (true) {
        console->Task();
        vTaskDelay(pdMS_TO_TICKS(20));  // 20ms delay - matches main loop period, keeps console responsive
    }
}

SerialConsole::SerialConsole(config::DeviceConfig* config)
    : input_(std::make_unique<InputHandler>(*this)),
      dispatcher_(std::make_unique<CommandDispatcher>(*this)),
      output_(std::make_unique<OutputBuffer>()),
      config_(config) {
}

SerialConsole::~SerialConsole() = default;

std::string SerialConsole::GetPrompt() const {
    // Get firmware version (git commit hash) from app description
    const esp_app_desc_t* app_desc = esp_app_get_description();
    const char* version = app_desc ? app_desc->version : "unknown";

    // Format: "CALLSIGN - commit> " or "ESP32-Keyer - commit> "
    std::string base;
    if (config_ && config_->general.callsign[0] != '\0') {
        base = std::string(config_->general.callsign);
    } else {
        base = "ESP32-Keyer";
    }

    return base + " - " + version + "> ";
}

void SerialConsole::Init() {
    input_->init();

    // Create FreeRTOS task for console processing
    BaseType_t result = xTaskCreate(
        console_task_entry,
        "serial_console",       // Task name
        8192,                   // Stack size: 8KB (safe for C++ objects, vectors, strings + USB-CDC)
        this,                   // Task parameter (this pointer)
        5,                      // Priority: same as USB heartbeat task
        nullptr                 // Task handle (not needed)
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create console task");
    } else {
        ESP_LOGI(TAG, "Console initialized on USB-CDC1 (COM7)");
    }
}

void SerialConsole::RegisterCommand(const std::string& verb,
                                    CommandHandler handler,
                                    const std::string& help,
                                    TabCompleteHandler tab_complete) {
    dispatcher_->registerCommand(verb, handler, help, tab_complete);
}

void SerialConsole::Task() {
    input_->process();
}

void SerialConsole::Print(const std::string& str) {
    tinyusb_cdcacm_write_queue(CONSOLE_CDC_PORT,
                               reinterpret_cast<const uint8_t*>(str.c_str()),
                               str.length());
    tinyusb_cdcacm_write_flush(CONSOLE_CDC_PORT, 0);  // Non-blocking flush
    output_->addLine(str);
}

void SerialConsole::Printf(const char* fmt, ...) {
    char buf[256];  // 256 bytes - sufficient for typical console messages
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Print(buf);
}

void SerialConsole::ProcessLine(const std::string& line) {
    // Tokenize command line into arguments
    std::vector<std::string> args;
    std::istringstream iss(line);
    std::string token;

    while (iss >> token && args.size() < MAX_ARGS) {
        args.push_back(token);
    }

    // Dispatch to command handler
    if (!args.empty()) {
        dispatcher_->dispatch(args);
    }
}

std::vector<Command>& SerialConsole::GetCommands() {
    return dispatcher_->getCommands();
}

} // namespace ui
