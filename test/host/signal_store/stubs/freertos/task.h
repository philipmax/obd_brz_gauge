#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define portTICK_PERIOD_MS 1u
TickType_t xTaskGetTickCount(void);
