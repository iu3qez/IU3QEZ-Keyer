/**
 * USB initialization using esp_tinyusb 2.x API
 * Upgraded from 1.x for modern API
 * 
 * This module manages dual CDC ACM interfaces:
 * - CDC0 (COM8): Debug logs via esp_log_set_vprintf hook
 * - CDC1 (COM7): Console/TUI interface
 */

#include "app/usb_early_init.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "lwip/inet.h"
#include "tinyusb.h"
#include "tusb.h"  // Low-level TinyUSB API
#include "tinyusb_cdc_acm.h"  // New API from esp_tinyusb 2.x
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// Logging tag
constexpr char kLogTag[] = "usb_early";

// CDC port assignments
constexpr tinyusb_cdcacm_itf_t kDebugPort = TINYUSB_CDC_ACM_0;    // COM8 - debug logs
constexpr tinyusb_cdcacm_itf_t kServicePort = TINYUSB_CDC_ACM_1;  // COM7 - console

}  // namespace

// Module-level state (file scope, internal linkage)
// These variables track USB CDC state across callbacks and tasks
static struct {
    TaskHandle_t heartbeat_task;       // Background task for CDC monitoring
    bool cdc_ready[2];                  // Track if terminal connected (DTR+RTS) per port
    vprintf_like_t prev_vprintf;        // Original vprintf for chaining
    bool log_hook_installed;            // Track if esp_log hook is active
    uint32_t hook_call_count;           // DEBUG: Count hook calls
    uint32_t hook_write_count;          // DEBUG: Count successful writes
} g_usb_state = {
    .heartbeat_task = nullptr,
    .cdc_ready = {false, false},
    .prev_vprintf = nullptr,
    .log_hook_installed = false,
    .hook_call_count = 0,
    .hook_write_count = 0,
};

//==============================================================================
// Public API (C linkage for cross-module access)
//==============================================================================

/**
 * Check if a CDC terminal is connected (based on DTR+RTS signals from callback)
 * This is more reliable than tud_cdc_n_connected() which may lag behind actual state.
 * 
 * @param port_num CDC port number (0=debug, 1=console)
 * @return true if terminal connected and ready
 */
extern "C" bool usb_cdc_is_terminal_connected(int port_num) {
    if (port_num < 0 || port_num >= 2) {
        return false;
    }
    return g_usb_state.cdc_ready[port_num];
}

/**
 * DEBUG: Get ESP_LOG hook statistics
 * @param hook_calls Output: number of times hook was called
 * @param hook_writes Output: number of successful writes to CDC
 */
extern "C" void usb_get_hook_stats(uint32_t* hook_calls, uint32_t* hook_writes) {
    if (hook_calls) {
        *hook_calls = g_usb_state.hook_call_count;
    }
    if (hook_writes) {
        *hook_writes = g_usb_state.hook_write_count;
    }
}

//==============================================================================
// Internal helper functions
//==============================================================================

namespace {

// Heartbeat task - sends periodic messages to verify system is alive
static void usb_heartbeat_task(void* arg) {
    (void)arg;
    uint32_t counter = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));  // 10 seconds

        // Get WiFi IP address
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        char ip_str[16] = "no_wifi";

        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            }
        }

        // Log heartbeat with WiFi IP and hook statistics
        ESP_LOGI(kLogTag, "HEARTBEAT #%lu @ %lld us | WiFi: %s | Hook: %s (calls=%lu, writes=%lu)",
                 counter++, esp_timer_get_time(), ip_str,
                 g_usb_state.log_hook_installed ? "ON" : "OFF",
                 (unsigned long)g_usb_state.hook_call_count,
                 (unsigned long)g_usb_state.hook_write_count);
    }
}

// Line state callback - detects when terminal connects (2.x API)
static void cdc_line_state_changed_callback(int itf, cdcacm_event_t *event) {
    if (!event || event->type != CDC_EVENT_LINE_STATE_CHANGED) {
        return;
    }

    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    bool connected = dtr && rts;

    if (itf >= 0 && itf < 2) {
        g_usb_state.cdc_ready[itf] = connected;
    }

    // No banners - TUI will display on CDC1 when initialized
}

// Log redirect hook - sends ESP_LOG to CDC
static int usb_debug_log_vprintf(const char *fmt, va_list args) {
    g_usb_state.hook_call_count++;  // DEBUG: Track hook calls
    
    int ret = 0;

    // Chain to previous vprintf (usually UART)
    va_list args_for_prev;
    va_copy(args_for_prev, args);
    if (g_usb_state.prev_vprintf) {
        ret = g_usb_state.prev_vprintf(fmt, args_for_prev);
    }
    va_end(args_for_prev);

    // TODO: Removed cdc_ready[kDebugPort] check to fix ESP_LOGI() not appearing on COM8.
    // Problem: cdc_ready becomes true only AFTER line_state_changed_callback is called,
    // which happens when user opens COM8. If COM8 is opened after usb_early_init(),
    // all ESP_LOGI() messages between init and connection were lost.
    // TinyUSB buffers writes internally, so writing before connection is safe - data
    // appears when terminal connects. This allows ESP_LOGI() in Initialize() to be visible.
    // Proper fix: Pre-set cdc_ready after tinyusb_cdcacm_init() or use TinyUSB's tud_cdc_n_connected()
    if (!tinyusb_cdcacm_initialized(kDebugPort)) {
        return ret;
    }

    char buffer[256];
    va_list args_for_cdc;
    va_copy(args_for_cdc, args);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args_for_cdc);
    va_end(args_for_cdc);

    if (len > 0) {
        size_t to_write = static_cast<size_t>(len);
        if (to_write >= sizeof(buffer)) {
            to_write = sizeof(buffer) - 1;
        }

        // Convert LF to CRLF for proper terminal display
        // vsnprintf doesn't respect CONFIG_NEWLIB_STDOUT_LINE_ENDING_CRLF,
        // so we must convert manually (same as UART1 log hook)
        static char crlf_buffer[512];  // Double size for worst case (all LF)
        size_t dst_idx = 0;
        for (size_t i = 0; i < to_write && dst_idx < sizeof(crlf_buffer) - 1; i++) {
            if (buffer[i] == '\n') {
                crlf_buffer[dst_idx++] = '\r';  // Add CR before LF
            }
            crlf_buffer[dst_idx++] = buffer[i];
        }

        size_t written = tinyusb_cdcacm_write_queue(kDebugPort,
                                                    reinterpret_cast<const uint8_t *>(crlf_buffer),
                                                    dst_idx);
        if (written > 0) {
            g_usb_state.hook_write_count++;  // DEBUG: Track successful writes
            tinyusb_cdcacm_write_flush(kDebugPort, 0);
        }

        // UART1 output is handled by previous vprintf hook (installed early in ApplicationController)
    }

    return ret;
}

}  // namespace

//==============================================================================
// Public initialization function
//==============================================================================

esp_err_t usb_early_init() {
    // Config using new 2.x API - nullptr uses Kconfig defaults for dual CDC
    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,       // Let esp_tinyusb configure internal PHY
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 4096,
            .priority = 5,
            .xCoreID = 0,              // Pin to core 0 (same as USB ISR)
        },
        .descriptor = {
            .device = nullptr,         // Use Kconfig defaults
            .qualifier = nullptr,
            .string = nullptr,
            .string_count = 0,
            .full_speed_config = nullptr,  // Use Kconfig defaults for dual CDC
            .high_speed_config = nullptr,
        },
        .event_cb = nullptr,
        .event_arg = nullptr,
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    // Initialize CDC ACM 0 (debug logs) with line state callback using 2.x API
    tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = &cdc_line_state_changed_callback,
        .callback_line_coding_changed = nullptr,
    };

    err = tinyusb_cdcacm_init(&cdc_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    // Initialize CDC ACM 1 (service/timeline) with line state callback
    cdc_cfg.cdc_port = TINYUSB_CDC_ACM_1;
    err = tinyusb_cdcacm_init(&cdc_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    // Install log hook to redirect ESP_LOG to CDC0
    if (!g_usb_state.log_hook_installed) {
        g_usb_state.prev_vprintf = esp_log_set_vprintf(&usb_debug_log_vprintf);
        g_usb_state.log_hook_installed = true;
    }

    // Create heartbeat task to verify CDC is working
    if (!g_usb_state.heartbeat_task) {
        xTaskCreate(usb_heartbeat_task,
                   "usb_heartbeat",
                   4096,  // Stack: 4KB (was 2KB, but task uses ~2.2KB causing overflow)
                   nullptr,
                   5,  // Priority
                   &g_usb_state.heartbeat_task);
    }

    ESP_LOGI(kLogTag, "TinyUSB dual CDC initialized with log hook and heartbeat");
    return ESP_OK;
}