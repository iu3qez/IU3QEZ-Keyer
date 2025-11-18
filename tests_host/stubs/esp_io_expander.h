#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_io_expander* esp_io_expander_handle_t;

typedef enum {
  IO_EXPANDER_OUTPUT = 0,
} esp_io_expander_direction_t;

esp_err_t esp_io_expander_new_i2c_tca95xx_16bit(void* bus_handle,
                                                uint32_t i2c_address,
                                                esp_io_expander_handle_t* handle);
esp_err_t esp_io_expander_del(esp_io_expander_handle_t handle);
esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t handle,
                                  uint32_t mask,
                                  esp_io_expander_direction_t direction);
esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t handle,
                                    uint32_t mask,
                                    uint32_t level);

#ifdef __cplusplus
}
#endif
