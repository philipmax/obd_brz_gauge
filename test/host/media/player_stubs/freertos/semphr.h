#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void *xSemaphoreGetMutexHolder(SemaphoreHandle_t sem);
