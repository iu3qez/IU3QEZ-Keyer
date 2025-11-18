#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

#define pdPASS 1
#define pdFAIL 0

#define pdMS_TO_TICKS(ms) (ms)

static const BaseType_t tskNO_AFFINITY = -1;

#ifdef __cplusplus
}
#endif
