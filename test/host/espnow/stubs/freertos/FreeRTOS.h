#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef struct { bool locked; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {false}
#define configTICK_RATE_HZ 1000u
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void ble_test_enter(portMUX_TYPE *mux);
void ble_test_exit(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) ble_test_enter(mux)
#define portEXIT_CRITICAL(mux) ble_test_exit(mux)
