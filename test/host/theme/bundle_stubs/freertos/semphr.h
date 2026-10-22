#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
BaseType_t xSemaphoreTake(SemaphoreHandle_t mux, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mux);
