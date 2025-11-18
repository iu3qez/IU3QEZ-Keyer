#include "app/boot_failure_tracker.hpp"

extern "C" {
#include "esp_log.h"
#include "esp_attr.h"
}

namespace app {

namespace {
constexpr char kLogTag[] = "boot_failure";

// RTC memory variable persists across soft resets but not power cycles
// Initialized to 0 on power-on, retains value across esp_restart()
RTC_DATA_ATTR static uint8_t rtc_boot_fail_count = 0;
}  // namespace

uint8_t IncrementBootFailureCount() {
  rtc_boot_fail_count++;

  ESP_LOGW(kLogTag, "Boot failure count incremented: %d/%d",
           rtc_boot_fail_count, kBootFailureThreshold);

  if (rtc_boot_fail_count >= kBootFailureThreshold) {
    ESP_LOGE(kLogTag, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGE(kLogTag, "║              BOOTLOOP DETECTED                           ║");
    ESP_LOGE(kLogTag, "╠══════════════════════════════════════════════════════════╣");
    ESP_LOGE(kLogTag, "║ Consecutive boot failures: %d (threshold: %d)             ║",
             rtc_boot_fail_count, kBootFailureThreshold);
    ESP_LOGE(kLogTag, "║                                                          ║");
    ESP_LOGE(kLogTag, "║ Device will enter SAFE MODE for recovery                ║");
    ESP_LOGE(kLogTag, "║                                                          ║");
    ESP_LOGE(kLogTag, "║ Recovery options:                                        ║");
    ESP_LOGE(kLogTag, "║  1. Enter bootloader (automatic)                         ║");
    ESP_LOGE(kLogTag, "║  2. Delete FACTORY_RESET.TXT from bootloader drive      ║");
    ESP_LOGE(kLogTag, "║  3. Device will erase NVS and restart with defaults      ║");
    ESP_LOGE(kLogTag, "╚══════════════════════════════════════════════════════════╝");
  }

  return rtc_boot_fail_count;
}

bool IsBootloopDetected() {
  return rtc_boot_fail_count >= kBootFailureThreshold;
}

void ClearBootFailureCount() {
  if (rtc_boot_fail_count > 0) {
    ESP_LOGI(kLogTag, "Boot successful - clearing failure count (was: %d)", rtc_boot_fail_count);
    rtc_boot_fail_count = 0;
  }
}

uint8_t GetBootFailureCount() {
  return rtc_boot_fail_count;
}

}  // namespace app
