#pragma once

#include "esp_err.h"

#define ESP_RETURN_ON_ERROR(expr, tag, format, ...)   \
  do {                                                \
    esp_err_t esp_macro_err_rc = (expr);              \
    if (esp_macro_err_rc != ESP_OK) {                 \
      (void)(tag);                                    \
      (void)(format);                                 \
      return esp_macro_err_rc;                        \
    }                                                 \
  } while (0)
