#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct pp_case pp_case_t;
/* Test/benchmark only: the production engine has no probe hooks. */
pp_case_t *pp_create(unsigned signals, unsigned pattern, uint32_t seed, bool mixed);
void pp_destroy(pp_case_t *c);
bool pp_verify(pp_case_t *c, uint32_t now);
void pp_transition(pp_case_t *c, uint32_t seed);
uint32_t pp_linear(pp_case_t *c, uint32_t now);
uint32_t pp_mask(pp_case_t *c, uint32_t now);
