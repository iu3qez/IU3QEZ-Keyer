#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;

static const gpio_num_t GPIO_NUM_NC = -1;
static const gpio_num_t GPIO_NUM_0 = 0;

typedef enum {
  GPIO_MODE_DISABLE = 0,
  GPIO_MODE_INPUT = 1,
  GPIO_MODE_OUTPUT = 2,
} gpio_mode_t;

typedef enum {
  GPIO_INTR_DISABLE = 0,
  GPIO_INTR_POSEDGE = 1,
  GPIO_INTR_NEGEDGE = 2,
  GPIO_INTR_ANYEDGE = 3,
} gpio_int_type_t;

typedef enum {
  GPIO_PULLUP_DISABLE = 0,
  GPIO_PULLUP_ENABLE = 1,
} gpio_pullup_t;

typedef enum {
  GPIO_PULLDOWN_DISABLE = 0,
  GPIO_PULLDOWN_ENABLE = 1,
} gpio_pulldown_t;

typedef struct {
  uint64_t pin_bit_mask;
  gpio_mode_t mode;
  gpio_pullup_t pull_up_en;
  gpio_pulldown_t pull_down_en;
  gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void*);

esp_err_t gpio_config(const gpio_config_t* config);
esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_set_level(gpio_num_t gpio_num, int level);
int gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void* args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);
esp_err_t gpio_install_isr_service(int flags);

#define ESP_INTR_FLAG_IRAM 0x01

#ifdef __cplusplus
}
#endif
