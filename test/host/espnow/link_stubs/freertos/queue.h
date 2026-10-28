#pragma once
#include "freertos/FreeRTOS.h"
#include <stddef.h>
typedef struct { uint8_t *bytes; size_t size; unsigned capacity, count, head; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size,
                                 uint8_t *bytes, StaticQueue_t *storage);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
