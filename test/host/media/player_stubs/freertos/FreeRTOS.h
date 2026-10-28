#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef struct { unsigned depth; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define BIT0 (1u << 0)
#define BIT1 (1u << 1)
#define BIT2 (1u << 2)
#define portMUX_INITIALIZE(lock) ((lock)->depth = 0)
void player_test_enter(portMUX_TYPE *lock);
void player_test_exit(portMUX_TYPE *lock);
#define taskENTER_CRITICAL(lock) player_test_enter(lock)
#define taskEXIT_CRITICAL(lock) player_test_exit(lock)
