// Bootloader Entry Logic Implementation
// Copyright (c) 2025 IU3QEZ

#include "app/bootloader_entry.hpp"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "bootloader_entry";

namespace app {

// RTC memory variable survives soft reset but not power cycle
// Used to signal bootloader entry across reboot boundary
RTC_DATA_ATTR static uint32_t rtc_bootloader_magic = 0;

void EnterBootloaderMode() {
  ESP_LOGI(TAG, "Entering UF2 bootloader mode...");
  ESP_LOGI(TAG, "Device will appear as USB drive 'KEYERBOOT'");
  ESP_LOGI(TAG, "Drag firmware.uf2 file to update");

  // Write magic value to RTC memory
  rtc_bootloader_magic = kBootloaderMagic;

  ESP_LOGI(TAG, "Magic value written: 0x%08lX", rtc_bootloader_magic);
  ESP_LOGI(TAG, "Restarting...");

  // Give time for log messages to flush
  vTaskDelay(pdMS_TO_TICKS(100));

  // Trigger soft reset
  esp_restart();

  // Never returns
}

bool CheckBootloaderFlag() {
  if (rtc_bootloader_magic == kBootloaderMagic) {
    ESP_LOGI(TAG, "Bootloader magic detected: 0x%08lX", rtc_bootloader_magic);
    ESP_LOGI(TAG, "Bootloader entry requested");

    // Clear magic to prevent sticky behavior
    ClearBootloaderFlag();

    return true;
  }

  return false;
}

void ClearBootloaderFlag() {
  rtc_bootloader_magic = 0;
  ESP_LOGD(TAG, "Bootloader magic cleared");
}

}  // namespace app
