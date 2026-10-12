#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Scan /OBD/IMPORT directory for vehicle profiles and control files
 * @return ESP_OK on success
 */
esp_err_t sd_import_scan(void);

#ifdef __cplusplus
}
#endif
