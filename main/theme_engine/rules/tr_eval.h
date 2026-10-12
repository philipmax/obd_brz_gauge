#pragma once
// ============================================================
//  tr_eval.h — evaluate a compiled rules program
//
//  Mirrors evaluatePageRules() / isRuleActive() of the editor's
//  ruleEngine.ts with a persistent runtime (the editor's sticky
//  Map): per-condition directional hysteresis latch, debounce as
//  on-delay, priority-ascending stable order, overrides recomputed
//  from base on every call.
// ============================================================

#include "tr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Clear every latch and debounce timer (page build / theme reload).
void tr_reset(tr_program_t *prog);

// One tick. Evaluates every rule in page order, updates the runtime and
// returns the override table (owned by prog, valid until the next call).
const tr_result_t *tr_eval(tr_program_t *prog, const tr_inputs_t *in);

// isRuleActive() for one rule: trigger/conditions only. Updates that rule's
// latches, never its debounce state.
bool tr_rule_active(tr_program_t *prog, uint8_t rule_idx, const tr_inputs_t *in);

// Override of a target, or NULL when no active action touched it.
const tr_override_t *tr_result_override(const tr_program_t *prog, uint16_t target);

#ifdef __cplusplus
}
#endif
