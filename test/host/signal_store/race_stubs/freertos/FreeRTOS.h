#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void ss_test_enter(portMUX_TYPE *);
void ss_test_exit(portMUX_TYPE *);
#define portENTER_CRITICAL(mux) ss_test_enter(mux)
#define portEXIT_CRITICAL(mux) ss_test_exit(mux)
