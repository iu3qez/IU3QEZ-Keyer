// main.cpp - ESP-IDF firmware entry point for Keyer QRS2HST.
//
// This file contains only the minimal ESP-IDF bootstrap code required to start the application.
// All subsystem initialization, configuration management, and main loop logic has been delegated
// to the ApplicationController class (components/app/) per style guide requirement:
// "Main file must be light and not monolithic."
//
// Responsibilities of app_main():
// 1. Log startup banner
// 2. Instantiate ApplicationController
// 3. Delegate Initialize() and Run() to controller
// 4. Handle catastrophic controller failures (abort)
//
// All hardware abstraction, keying logic, audio services, console commands, and configuration
// management are encapsulated in dedicated components with well-defined interfaces.

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app/application_controller.hpp"

namespace {
constexpr char kLogTag[] = "app_main";  // Logging tag for firmware entry point.
}  // namespace

extern "C" void app_main(void) {
  // Create application controller (owns all subsystems via RAII).
  app::ApplicationController controller;

  // Initialize all subsystems in dependency order.
  if (!controller.Initialize()) {
    abort();
  }

  controller.Run();

  // Unreachable under normal circumstances (Run() contains infinite loop).
  ESP_LOGE(kLogTag, "ApplicationController::Run() returned unexpectedly - halting");
  abort();
}
