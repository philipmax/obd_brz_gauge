#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize config partition (LittleFS)
 * @return ESP_OK on success
 */
esp_err_t config_partition_init(void);

/**
 * @brief Unmount config partition
 * @return ESP_OK on success
 */
esp_err_t config_partition_deinit(void);

/**
 * @brief Check if config partition is mounted
 * @return true if mounted
 */
bool config_partition_is_mounted(void);

/**
 * @brief Get config partition mount point
 * @return Mount point path (e.g., "/config")
 */
const char *config_partition_get_mount_point(void);

/**
 * @brief Ensure directory structure exists
 * @return ESP_OK on success
 */
esp_err_t config_partition_ensure_directories(void);

/**
 * @brief Format config partition
 * @return ESP_OK on success
 */
esp_err_t config_partition_format(void);

/**
 * @brief Get partition usage info
 * @param total_bytes Output: total bytes
 * @param used_bytes Output: used bytes
 * @return ESP_OK on success
 */
esp_err_t config_partition_get_info(size_t *total_bytes, size_t *used_bytes);

/**
 * @brief Check if write would exceed quota (75%)
 * @param bytes_to_write Number of bytes to write
 * @return true if quota allows
 */
bool config_partition_check_quota(size_t bytes_to_write);

/**
 * @brief Sync config partition to SD mirror
 * @param sd_mirror_path Path to SD mirror directory
 * @return ESP_OK on success
 */
esp_err_t config_partition_sync_to_sd(const char *sd_mirror_path);

/**
 * @brief Restore config partition from SD mirror
 * @param sd_mirror_path Path to SD mirror directory
 * @return ESP_OK on success
 */
esp_err_t config_partition_restore_from_sd(const char *sd_mirror_path);

// Aliases for app_main compatibility
esp_err_t config_part_init(void);
esp_err_t config_part_format(void);
esp_err_t config_part_restore_from_sd(void);
esp_err_t config_part_mirror_to_sd(void);

#ifdef __cplusplus
}
#endif
