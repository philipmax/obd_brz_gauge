#ifndef RULE_ENGINE_H
#define RULE_ENGINE_H

#include "signal_store.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Condition operators
typedef enum {
    OP_GT = 0,
    OP_GTE,
    OP_LT,
    OP_LTE,
    OP_EQ,
    OP_NE
} rule_op_t;

// Trigger types
typedef enum {
    TRIGGER_CONDITION = 0,
    TRIGGER_TIMER,
    TRIGGER_CHANGE
} trigger_type_t;

// Combine modes
typedef enum {
    COMBINE_ALL = 0,
    COMBINE_ANY
} combine_mode_t;

// Action types
typedef enum {
    ACTION_SET_VISIBLE = 0,
    ACTION_SET_COLOR,
    ACTION_SET_ASSET,
    ACTION_SET_TEXT
} action_type_t;

// Condition
typedef struct {
    signal_id_t source;
    rule_op_t op;
    double threshold;
    double bandwidth;  // hysteresis bandwidth
} rule_condition_t;

// Action
typedef struct {
    action_type_t type;
    union {
        bool visible;
        uint32_t color;
        char text[64];
        uint32_t asset_id;
    } value;
} rule_action_t;

// Rule definition
typedef struct {
    uint8_t enabled;  // Only false (0) disables
    trigger_type_t trigger_type;
    uint32_t trigger_interval;  // For TRIGGER_TIMER
    signal_id_t trigger_source;  // For TRIGGER_CHANGE
    combine_mode_t combine;
    uint32_t debounce_ms;
    uint8_t priority;

    rule_condition_t conditions[4];
    uint8_t condition_count;

    rule_action_t actions[8];
    uint8_t action_count;
} rule_t;

// Evaluation result
typedef struct {
    uint8_t rule_index;
    rule_action_t actions[8];
    uint8_t action_count;
} rule_result_t;

/**
 * Initialize rule engine
 */
void rule_engine_init(void);

/**
 * Add a rule (max 16)
 */
esp_err_t rule_engine_add_rule(const rule_t *rule);

/**
 * Called when page changes: reset latch/debounce/change states
 */
void rule_engine_page_changed(void);

/**
 * Evaluate all rules and return firing rules
 * @param now_ms Current time in milliseconds
 * @param results Output array (min size 16)
 * @param result_count Number of results
 */
void rule_engine_evaluate(uint32_t now_ms, rule_result_t *results, uint8_t *result_count);

/**
 * Get current rule count
 */
uint8_t rule_engine_get_count(void);

/**
 * Clear all rules
 */
void rule_engine_clear(void);

#ifdef __cplusplus
}
#endif

#endif // RULE_ENGINE_H
