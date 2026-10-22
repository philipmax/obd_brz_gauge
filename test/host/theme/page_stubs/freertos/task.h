#pragma once
#include "../platform.h"
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio, void *handle);
void vTaskDelete(void *task);
