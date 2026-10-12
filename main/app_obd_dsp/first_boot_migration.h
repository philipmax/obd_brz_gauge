#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run first-boot migration (partition table v1 -> v2)
 * @return ESP_OK on success
 */
esp_err_t first_boot_migration_run(void);

#ifdef __cplusplus
}
#endif
