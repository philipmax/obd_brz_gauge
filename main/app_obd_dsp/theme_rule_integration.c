/**
 * theme_rule_integration.c
 *
 * Integrate rule engine with theme system and LVGL
 * Called from my_timerMain to apply rule actions to theme elements
 */

#include "theme_rule_integration.h"
#include "rule_engine.h"
#include "signal_store.h"
#include "../theme_engine/theme_interface.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "theme_rules";

// Track applied state per element to avoid redundant LVGL calls
typedef struct {
    bool visible;
    uint32_t color;
    uint32_t bg_color;
    bool has_color;
    bool has_bg_color;
    char text[64];
    bool has_text;
} element_state_t;

#define MAX_ELEMENTS 32
static element_state_t s_element_states[MAX_ELEMENTS];
static bool s_states_initialized = false;

void theme_rule_integration_init(void)
{
    rule_engine_init();
    memset(s_element_states, 0, sizeof(s_element_states));
    s_states_initialized = false;
    ESP_LOGI(TAG, "Theme rule integration initialized");
}

void theme_rule_integration_page_changed(void)
{
    rule_engine_page_changed();
    memset(s_element_states, 0, sizeof(s_element_states));
    s_states_initialized = false;
}

// Apply actions to LVGL objects via theme engine
static void apply_action_to_element(lv_obj_t *obj, const theme_element_t *elem,
                                     const rule_action_t *action, element_state_t *state)
{
    if (!obj || !elem || !action) return;

    switch (action->type) {
        case ACTION_SET_VISIBLE: {
            bool visible = action->value.visible;
            if (state->visible != visible || !s_states_initialized) {
                if (visible) {
                    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
                }
                state->visible = visible;
            }
            break;
        }

        case ACTION_SET_COLOR: {
            uint32_t color = action->value.color;

            // Apply based on element type
            switch (elem->type) {
                case THEME_ELEM_ARC:
                case THEME_ELEM_BAR:
                    // Set both foreground and background color
                    if (!state->has_color || state->color != color || !s_states_initialized) {
                        lv_obj_set_style_arc_color(obj, lv_color_hex(color), LV_PART_INDICATOR);
                        lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
                        state->color = color;
                        state->has_color = true;
                    }
                    break;

                case THEME_ELEM_IMAGE:
                case THEME_ELEM_POINTER:
                    // Use recolor with opa=191 (0.75)
                    if (!state->has_color || state->color != color || !s_states_initialized) {
                        lv_obj_set_style_img_recolor(obj, lv_color_hex(color), LV_PART_MAIN);
                        lv_obj_set_style_img_recolor_opa(obj, 191, LV_PART_MAIN);
                        state->color = color;
                        state->has_color = true;
                    }
                    break;

                case THEME_ELEM_LABEL:
                    // Set text color
                    if (!state->has_color || state->color != color || !s_states_initialized) {
                        lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
                        state->color = color;
                        state->has_color = true;
                    }
                    break;

                default:
                    break;
            }
            break;
        }

        case ACTION_SET_ASSET:
            // Asset switching via theme engine (change image descriptor)
            // Not implemented in this minimal version
            ESP_LOGD(TAG, "SET_ASSET not yet implemented");
            break;

        case ACTION_SET_TEXT: {
            // Only applies to static labels (data-bound labels ignore this)
            if (elem->type == THEME_ELEM_LABEL && elem->data_source == SIG_NONE) {
                if (!state->has_text || strcmp(state->text, action->value.text) != 0 || !s_states_initialized) {
                    lv_label_set_text(obj, action->value.text);
                    strncpy(state->text, action->value.text, sizeof(state->text) - 1);
                    state->has_text = true;
                }
            }
            break;
        }
    }
}

void theme_rule_integration_tick(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // Evaluate rules
    rule_result_t results[16];
    uint8_t result_count = 0;
    rule_engine_evaluate(now_ms, results, &result_count);

    // Get current theme page elements
    // (This is a placeholder - actual implementation would query theme_loader
    //  for the current page's element list and LVGL objects)

    // For each firing rule, apply actions to corresponding elements
    for (uint8_t i = 0; i < result_count; i++) {
        const rule_result_t *res = &results[i];

        for (uint8_t j = 0; j < res->action_count; j++) {
            const rule_action_t *action = &res->actions[j];

            // TODO: Look up element by ID from theme page
            // For now this is a stub showing the structure
            // Real implementation needs theme_loader API to get element list

            // apply_action_to_element(obj, elem, action, &s_element_states[elem_idx]);
        }
    }

    s_states_initialized = true;
}

esp_err_t theme_rule_integration_load_sidecar(const char *theme_id)
{
    // Load rules from /config/themes/<theme_id>/sidecar.json
    // Parse JSON and call rule_engine_add_rule() for each rule

    // This is a placeholder - actual implementation would:
    // 1. Open file from config partition
    // 2. Parse JSON with cJSON
    // 3. Convert editor format (camelCase) to rule_t structs
    // 4. Handle element ID matching

    ESP_LOGI(TAG, "Sidecar loading for theme '%s' not yet implemented", theme_id);
    return ESP_ERR_NOT_SUPPORTED;
}
