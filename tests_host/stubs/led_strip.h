#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int strip_gpio_num;
  uint32_t max_leds;
  int led_model;
  int color_component_format;
  struct {
    bool invert_out;
  } flags;
} led_strip_config_t;

typedef struct {
  int clk_src;
  uint32_t resolution_hz;
  uint32_t mem_block_symbols;
  struct {
    bool with_dma;
  } flags;
} led_strip_rmt_config_t;

typedef struct led_strip_stub_t* led_strip_handle_t;

esp_err_t led_strip_new_rmt_device(const led_strip_config_t* config,
                                   const led_strip_rmt_config_t* rmt_config,
                                   led_strip_handle_t* out_handle);
esp_err_t led_strip_del(led_strip_handle_t handle);
esp_err_t led_strip_clear(led_strip_handle_t handle);
esp_err_t led_strip_set_pixel(led_strip_handle_t handle,
                              uint32_t index,
                              uint8_t red,
                              uint8_t green,
                              uint8_t blue);
esp_err_t led_strip_refresh(led_strip_handle_t handle);

#define LED_MODEL_WS2812 0
#define LED_STRIP_COLOR_COMPONENT_FMT_GRB 0
#define RMT_CLK_SRC_DEFAULT 0


#ifdef __cplusplus
}
#endif
