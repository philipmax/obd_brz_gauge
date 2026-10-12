#ifndef THEME_RULE_INTEGRATION_H
#define THEME_RULE_INTEGRATION_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize theme rule integration
 */
void theme_rule_integration_init(void);

/**
 * Called when theme page changes: reset rule states
 */
void theme_rule_integration_page_changed(void);

/**
 * Evaluate rules and apply actions to theme elements
 * Called from my_timerMain every frame
 */
void theme_rule_integration_tick(void);

/**
 * Load rule sidecar for a theme
 * @param theme_id Theme identifier
 * @return ESP_OK on success
 */
esp_err_t theme_rule_integration_load_sidecar(const char *theme_id);

#ifdef __cplusplus
}
#endif

#endif // THEME_RULE_INTEGRATION_H
