#pragma once

#include "FreeRTOS.h"

SemaphoreHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t mutex);
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait);
int xSemaphoreGive(SemaphoreHandle_t mutex);
