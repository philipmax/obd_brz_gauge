/**
 * rule_engine.c
 *
 * Theme rule engine core (pure C, host-compilable)
 * Implements the exact semantics of ruleEngine.ts from obd_theme_store@5a7484e
 *
 * - Hysteresis: each condition has its own latch state, direction-aware
 * - Combine: ALL (default) vs ANY
 * - Debounce: conduction delay (ms)
 * - Priority: ascending stable sort, later rules overwrite earlier ones
 * - Timer trigger: elapsed>0 && elapsed % max(100,I) < 120
 * - Change trigger: 280ms window per source
 * - All conditions evaluated (no short-circuit) to maintain latch consistency
 * - Missing/invalid sources treated as NaN: relational ops false, != true
 */

#include "rule_engine.h"
#include "signal_store.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#define MAX_RULES 16
#define MAX_CONDITIONS_PER_RULE 4
#define MAX_ACTIONS_PER_RULE 8

// Latch state for each condition (hysteresis)
typedef struct {
    bool latched;
    bool initialized;
} condition_latch_t;

// Debounce state
typedef struct {
    uint32_t first_true_ms;
    bool debouncing;
} debounce_state_t;

// Change trigger state
typedef struct {
    double last_value;
    uint32_t last_change_ms;
    bool initialized;
} change_state_t;

typedef struct {
    rule_t rule;
    condition_latch_t latches[MAX_CONDITIONS_PER_RULE];
    debounce_state_t debounce;
    change_state_t change;
    bool enabled;
} rule_state_t;

static rule_state_t s_rules[MAX_RULES];
static uint8_t s_rule_count = 0;
static uint32_t s_page_start_ms = 0;

// Stable sort by priority (ascending)
static void sort_rules_by_priority(void)
{
    // Insertion sort (stable)
    for (uint8_t i = 1; i < s_rule_count; i++) {
        rule_state_t temp = s_rules[i];
        int j = i - 1;
        while (j >= 0 && s_rules[j].rule.priority > temp.rule.priority) {
            s_rules[j + 1] = s_rules[j];
            j--;
        }
        s_rules[j + 1] = temp;
    }
}

void rule_engine_init(void)
{
    memset(s_rules, 0, sizeof(s_rules));
    s_rule_count = 0;
    s_page_start_ms = 0;
}

esp_err_t rule_engine_add_rule(const rule_t *rule)
{
    if (!rule || s_rule_count >= MAX_RULES) {
        return ESP_ERR_NO_MEM;
    }

    // Validate
    if (rule->condition_count > MAX_CONDITIONS_PER_RULE) return ESP_ERR_INVALID_ARG;
    if (rule->action_count > MAX_ACTIONS_PER_RULE) return ESP_ERR_INVALID_ARG;

    s_rules[s_rule_count].rule = *rule;
    s_rules[s_rule_count].enabled = (rule->enabled != 0); // Only false disables
    memset(s_rules[s_rule_count].latches, 0, sizeof(condition_latch_t) * MAX_CONDITIONS_PER_RULE);
    memset(&s_rules[s_rule_count].debounce, 0, sizeof(debounce_state_t));
    memset(&s_rules[s_rule_count].change, 0, sizeof(change_state_t));
    s_rule_count++;

    return ESP_OK;
}

void rule_engine_page_changed(void)
{
    s_page_start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // Clear all latch/debounce/change states
    for (uint8_t i = 0; i < s_rule_count; i++) {
        memset(s_rules[i].latches, 0, sizeof(condition_latch_t) * MAX_CONDITIONS_PER_RULE);
        memset(&s_rules[i].debounce, 0, sizeof(debounce_state_t));
        memset(&s_rules[i].change, 0, sizeof(change_state_t));
    }
}

// Get signal value as double (NaN if invalid)
static double get_signal_value(signal_id_t sig_id)
{
    int32_t value;
    if (!signal_store_get(sig_id, &value)) {
        return NAN;
    }
    return (double)value;
}

// Evaluate condition with hysteresis
static bool eval_condition_with_hyst(const rule_condition_t *cond, condition_latch_t *latch, double value)
{
    double threshold = cond->threshold;
    double bandwidth = cond->bandwidth;

    // NaN handling: relational ops false, != true
    if (isnan(value)) {
        switch (cond->op) {
            case OP_NE: return true;
            default: return false;
        }
    }

    bool raw_result;
    switch (cond->op) {
        case OP_GT: raw_result = (value > threshold); break;
        case OP_GTE: raw_result = (value >= threshold); break;
        case OP_LT: raw_result = (value < threshold); break;
        case OP_LTE: raw_result = (value <= threshold); break;
        case OP_EQ: raw_result = (fabs(value - threshold) < 0.001); break;
        case OP_NE: raw_result = (fabs(value - threshold) >= 0.001); break;
        default: return false;
    }

    // No hysteresis?
    if (bandwidth <= 0.0) {
        return raw_result;
    }

    // First evaluation
    if (!latch->initialized) {
        latch->latched = raw_result;
        latch->initialized = true;
        return raw_result;
    }

    // Hysteresis logic (direction-aware)
    // == and != are not affected by bandwidth
    if (cond->op == OP_EQ || cond->op == OP_NE) {
        return raw_result;
    }

    bool prev = latch->latched;

    // Determine direction
    bool rising = (cond->op == OP_GT || cond->op == OP_GTE);

    if (rising) {
        // Rising: trip at threshold, release at threshold - bandwidth
        if (!prev && value > threshold) {
            latch->latched = true;
        } else if (prev && value < (threshold - bandwidth)) {
            latch->latched = false;
        }
    } else {
        // Falling: trip at threshold, release at threshold + bandwidth
        if (!prev && value < threshold) {
            latch->latched = true;
        } else if (prev && value > (threshold + bandwidth)) {
            latch->latched = false;
        }
    }

    return latch->latched;
}

// Evaluate all conditions (no short-circuit)
static bool eval_conditions(rule_state_t *rs, uint32_t now_ms)
{
    uint8_t true_count = 0;

    for (uint8_t i = 0; i < rs->rule.condition_count; i++) {
        const rule_condition_t *cond = &rs->rule.conditions[i];
        double value = get_signal_value(cond->source);

        bool result = eval_condition_with_hyst(cond, &rs->latches[i], value);
        if (result) true_count++;
    }

    // Combine
    bool combined;
    if (rs->rule.combine == COMBINE_ANY) {
        combined = (true_count > 0);
    } else { // ALL (default)
        combined = (true_count == rs->rule.condition_count);
    }

    return combined;
}

// Evaluate trigger
static bool eval_trigger(rule_state_t *rs, uint32_t now_ms)
{
    switch (rs->rule.trigger_type) {
        case TRIGGER_TIMER: {
            uint32_t elapsed = now_ms - s_page_start_ms;
            uint32_t interval = (rs->rule.trigger_interval >= 100) ? rs->rule.trigger_interval : 100;
            return (elapsed > 0 && (elapsed % interval) < 120);
        }

        case TRIGGER_CHANGE: {
            signal_id_t src = rs->rule.trigger_source ? rs->rule.trigger_source :
                              (rs->rule.condition_count > 0 ? rs->rule.conditions[0].source : SIG_OBD_RPM);
            double value = get_signal_value(src);

            if (!rs->change.initialized) {
                rs->change.last_value = value;
                rs->change.last_change_ms = now_ms;
                rs->change.initialized = true;
                return false;
            }

            // Changed?
            if (!isnan(value) && fabs(value - rs->change.last_value) > 0.001) {
                rs->change.last_value = value;
                rs->change.last_change_ms = now_ms;
            }

            // Within 280ms window?
            return ((now_ms - rs->change.last_change_ms) <= 280);
        }

        default: // CONDITION or unknown
            return true;
    }
}

void rule_engine_evaluate(uint32_t now_ms, rule_result_t *results, uint8_t *result_count)
{
    if (!results || !result_count) return;

    *result_count = 0;

    // Sort by priority first (stable sort)
    sort_rules_by_priority();

    for (uint8_t i = 0; i < s_rule_count; i++) {
        rule_state_t *rs = &s_rules[i];

        if (!rs->enabled) continue;

        // Evaluate trigger
        bool trigger_active = eval_trigger(rs, now_ms);
        if (!trigger_active) {
            // Clear debounce when trigger inactive
            rs->debounce.debouncing = false;
            rs->debounce.first_true_ms = 0;
            continue;
        }

        // Evaluate conditions
        bool conditions_met = eval_conditions(rs, now_ms);

        if (!conditions_met) {
            // Clear debounce and latches when not met
            rs->debounce.debouncing = false;
            rs->debounce.first_true_ms = 0;
            continue;
        }

        // Debounce
        uint32_t debounce_ms = rs->rule.debounce_ms;
        if (debounce_ms > 0) {
            if (!rs->debounce.debouncing) {
                rs->debounce.debouncing = true;
                rs->debounce.first_true_ms = now_ms;
                continue; // Not yet
            }

            if ((now_ms - rs->debounce.first_true_ms) < debounce_ms) {
                continue; // Still debouncing
            }
        }

        // Rule fires: add to results
        if (*result_count < MAX_RULES) {
            results[*result_count].rule_index = i;
            results[*result_count].action_count = rs->rule.action_count;
            memcpy(results[*result_count].actions, rs->rule.actions,
                   sizeof(rule_action_t) * rs->rule.action_count);
            (*result_count)++;
        }
    }
}

uint8_t rule_engine_get_count(void)
{
    return s_rule_count;
}

void rule_engine_clear(void)
{
    memset(s_rules, 0, sizeof(s_rules));
    s_rule_count = 0;
}
