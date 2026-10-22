#pragma once
#include "freertos/task.h"
typedef struct sd_test_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t mutex);
