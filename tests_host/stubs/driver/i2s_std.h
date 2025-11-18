#pragma once

#include <cstdint>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int i2s_port_t;

typedef struct i2s_channel* i2s_chan_handle_t;

typedef enum {
  I2S_ROLE_MASTER = 0,
  I2S_ROLE_SLAVE = 1,
} i2s_role_t;

typedef struct {
  i2s_port_t id;
  i2s_role_t role;
  bool auto_clear;
} i2s_chan_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(port, role_value) \
  (i2s_chan_config_t{static_cast<i2s_port_t>(port), static_cast<i2s_role_t>(role_value), false})

typedef struct {
  int sample_rate_hz;
  int mclk_multiple;
} i2s_std_clk_config_t;

typedef struct {
  int data_bit_width;
  int slot_mode;
} i2s_std_slot_config_t;

typedef struct {
  gpio_num_t mclk;
  gpio_num_t bclk;
  gpio_num_t ws;
  gpio_num_t dout;
  gpio_num_t din;
  struct {
    bool mclk_inv;
    bool bclk_inv;
    bool ws_inv;
  } invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
  i2s_std_clk_config_t clk_cfg;
  i2s_std_slot_config_t slot_cfg;
  i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_value) \
  i2s_std_clk_config_t{static_cast<int>(sample_rate_value), 0}

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width_value, slot_mode_value) \
  i2s_std_slot_config_t{static_cast<int>(bit_width_value), static_cast<int>(slot_mode_value)}

enum {
  I2S_DATA_BIT_WIDTH_16BIT = 16,
};

enum {
  I2S_SLOT_MODE_STEREO = 2,
};

enum {
  I2S_MCLK_MULTIPLE_256 = 0,
};

esp_err_t i2s_new_channel(const i2s_chan_config_t* config,
                          i2s_chan_handle_t* tx_out,
                          i2s_chan_handle_t* rx_out);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t* std_cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle);
esp_err_t i2s_del_channel(i2s_chan_handle_t handle);

#ifdef __cplusplus
}
#endif
