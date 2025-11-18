#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_codec_dev* esp_codec_dev_handle_t;

typedef struct {
  uint8_t bits_per_sample;
  uint8_t channel;
  uint16_t channel_mask;
  uint32_t sample_rate;
  int mclk_multiple;
} esp_codec_dev_sample_info_t;

typedef enum {
  ESP_CODEC_DEV_TYPE_NONE = 0,
  ESP_CODEC_DEV_TYPE_OUT = 1,
} esp_codec_dev_type_t;

#define ESP_CODEC_DEV_OK 0

#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel) ((uint16_t)(1u << (channel)))

#ifdef __cplusplus
}
#endif
