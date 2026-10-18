#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct qc_case qc_case_t;
/* Private test TU compiles the real engine; no firmware hooks. */
qc_case_t *qc_create(unsigned head, unsigned count, uint32_t seed);
void qc_destroy(qc_case_t *c);
bool qc_push(qc_case_t *c, uint8_t kind, const char *cmd, uint16_t delay);
bool qc_pop(qc_case_t *c);
unsigned qc_count(const qc_case_t *c);
/* Timing wrappers execute real/reference push, pop and consume the step. */
uint32_t qc_format(qc_case_t *c, const char *cmd, uint32_t tick);
uint32_t qc_copy(qc_case_t *c, const char *cmd, uint32_t tick);

/* Actual state-command construction versus independent old formatting. */
bool qc_state(qc_case_t *c, uint8_t st, uint32_t val, int16_t req);
uint32_t qc_state_format(qc_case_t *c, uint8_t st, uint32_t val, int16_t req);
uint32_t qc_state_copy(qc_case_t *c, uint8_t st, uint32_t val, int16_t req);
