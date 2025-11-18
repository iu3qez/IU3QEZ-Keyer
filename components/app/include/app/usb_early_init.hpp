#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize USB early (right after NVS) to avoid conflicts
 * Based on working example from tmp/
 */
esp_err_t usb_early_init();

/**
 * Check if a CDC port has terminal connected (DTR+RTS from callback)
 * More reliable than tud_cdc_n_connected() which may lag
 * @param port_num 0 for CDC0 (debug), 1 for CDC1 (console)
 * @return true if terminal connected
 */
bool usb_cdc_is_terminal_connected(int port_num);

/**
 * DEBUG: Get ESP_LOG hook statistics
 * @param hook_calls Output: number of times hook was called
 * @param hook_writes Output: number of successful writes to CDC
 */
void usb_get_hook_stats(uint32_t* hook_calls, uint32_t* hook_writes);

#ifdef __cplusplus
}
#endif