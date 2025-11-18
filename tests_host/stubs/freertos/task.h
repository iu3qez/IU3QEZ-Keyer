#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task_func,
                                   const char* name,
                                   uint32_t stack_depth,
                                   void* params,
                                   UBaseType_t priority,
                                   TaskHandle_t* out_handle,
                                   BaseType_t core_id);

void vTaskDelete(TaskHandle_t task);
void vTaskDelay(uint32_t ticks);

#ifdef __cplusplus
}
#endif
