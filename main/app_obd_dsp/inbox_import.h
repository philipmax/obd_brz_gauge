#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Import new theme_0/bootmedia from flash partitions to SD
 * @return ESP_OK on success
 */
esp_err_t inbox_import_run(void);

#ifdef __cplusplus
}
#endif
