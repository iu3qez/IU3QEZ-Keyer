#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_CLK_SRC_DEFAULT 0

typedef int i2c_port_t;
typedef int i2c_port_num_t;

typedef struct i2c_master_bus_t* i2c_master_bus_handle_t;

typedef struct {
  i2c_port_t i2c_port;
  gpio_num_t sda_io_num;
  gpio_num_t scl_io_num;
  int clk_source;
  uint8_t glitch_ignore_cnt;
  int intr_priority;
  size_t trans_queue_depth;
  struct {
    uint32_t enable_internal_pullup : 1;
  } flags;
} i2c_master_bus_config_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* config, i2c_master_bus_handle_t* handle);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle);

#ifdef __cplusplus
}
#endif
