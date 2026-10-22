#pragma once
/* The production head reader uses the actual critical-section entry/exit API.
 * Host tests replace only that API with a pthread mutex and boundary hooks. */
#include <pthread.h>

typedef struct { pthread_mutex_t mutex; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { .mutex = PTHREAD_MUTEX_INITIALIZER }
void ring_test_enter(portMUX_TYPE *lock);
void ring_test_exit(portMUX_TYPE *lock);
#define taskENTER_CRITICAL(lock) ring_test_enter(lock)
#define taskEXIT_CRITICAL(lock) ring_test_exit(lock)
