#pragma once
#include "../platform.h"

static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return malloc(1); }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
