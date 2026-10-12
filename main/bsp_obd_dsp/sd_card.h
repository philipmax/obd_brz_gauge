#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SD card (SDMMC 1-bit mode)
 * @return ESP_OK on success
 */
esp_err_t sd_card_init(void);

/**
 * @brief Unmount and deinitialize SD card
 * @return ESP_OK on success
 */
esp_err_t sd_card_deinit(void);

/**
 * @brief Check if SD card is mounted
 * @return true if mounted
 */
bool sd_card_is_mounted(void);

/**
 * @brief Get SD card mount point
 * @return Mount point path (e.g., "/sdcard")
 */
const char *sd_card_get_mount_point(void);

/**
 * @brief Ensure /OBD directory structure exists
 * @return ESP_OK on success
 */
esp_err_t sd_card_ensure_directories(void);

/**
 * @brief Get total card capacity in bytes
 * @return Total bytes, or 0 if not mounted
 */
uint64_t sd_card_get_total_bytes(void);

/**
 * @brief Get free space in bytes
 * @return Free bytes, or 0 if not mounted
 */
uint64_t sd_card_get_free_bytes(void);

#ifdef __cplusplus
}
#endif
