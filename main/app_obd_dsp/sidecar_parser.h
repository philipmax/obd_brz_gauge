#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse .gaugeproj.json sidecar file
 * @param file_path Path to .gaugeproj.json file
 * @return ESP_OK on success
 */
esp_err_t sidecar_parse_file(const char *file_path);

/**
 * @brief Save parsed sidecar data to config partition
 * @param theme_id Theme ID
 * @return ESP_OK on success
 */
esp_err_t sidecar_save_to_config(const char *theme_id);

/**
 * @brief Get parsed theme ID
 * @return Theme ID string
 */
const char *sidecar_get_theme_id(void);

/**
 * @brief Get number of parsed elements
 * @return Element count
 */
int sidecar_get_element_count(void);

/**
 * @brief Clear parsed sidecar data
 */
void sidecar_clear(void);

#ifdef __cplusplus
}
#endif
