/*
 * Config Partition (LittleFS) Driver
 * Stores vehicle profiles, sidecar rules, and indexes
 */

#include "config_partition.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_littlefs.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "config_part";

#define CONFIG_MOUNT_POINT "/config"
#define CONFIG_PARTITION_LABEL "config"
#define CONFIG_MAX_FILES 10
#define CONFIG_QUOTA_PERCENT 75  // Use at most 75% of partition

static bool s_mounted = false;

esp_err_t config_partition_init(void) {
    if (s_mounted) {
        ESP_LOGW(TAG, "Config partition already mounted");
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = CONFIG_MOUNT_POINT,
        .partition_label = CONFIG_PARTITION_LABEL,
        .format_if_mount_failed = false,  // Never auto-format
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret == ESP_OK) {
        s_mounted = true;

        size_t total = 0, used = 0;
        esp_littlefs_info(CONFIG_PARTITION_LABEL, &total, &used);
        ESP_LOGI(TAG, "Config partition mounted: %zu KB total, %zu KB used", total / 1024, used / 1024);

        // Create directory structure
        config_partition_ensure_directories();
        return ESP_OK;
    }

    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "Config partition not found");
        return ret;
    }

    // Mount failed, try to format
    ESP_LOGW(TAG, "Mount failed (%s), formatting partition", esp_err_to_name(ret));
    ret = esp_littlefs_format(CONFIG_PARTITION_LABEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Try mount again after format
    ret = esp_vfs_littlefs_register(&conf);
    if (ret == ESP_OK) {
        s_mounted = true;
        ESP_LOGI(TAG, "Config partition formatted and mounted");
        config_partition_ensure_directories();
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to mount after format: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t config_partition_deinit(void) {
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_littlefs_unregister(CONFIG_PARTITION_LABEL);
    if (ret == ESP_OK) {
        s_mounted = false;
        ESP_LOGI(TAG, "Config partition unmounted");
    }
    return ret;
}

bool config_partition_is_mounted(void) {
    return s_mounted;
}

const char *config_partition_get_mount_point(void) {
    return CONFIG_MOUNT_POINT;
}

esp_err_t config_partition_ensure_directories(void) {
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *dirs[] = {
        CONFIG_MOUNT_POINT "/vehicles",
        CONFIG_MOUNT_POINT "/themes",
        CONFIG_MOUNT_POINT "/system"
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) {
            if (mkdir(dirs[i], 0755) != 0) {
                ESP_LOGW(TAG, "Failed to create %s", dirs[i]);
            } else {
                ESP_LOGD(TAG, "Created directory: %s", dirs[i]);
            }
        }
    }

    return ESP_OK;
}

esp_err_t config_partition_format(void) {
    if (s_mounted) {
        esp_vfs_littlefs_unregister(CONFIG_PARTITION_LABEL);
        s_mounted = false;
    }

    esp_err_t ret = esp_littlefs_format(CONFIG_PARTITION_LABEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Config partition formatted");

    // Remount after format
    return config_partition_init();
}

esp_err_t config_partition_get_info(size_t *total_bytes, size_t *used_bytes) {
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_littlefs_info(CONFIG_PARTITION_LABEL, total_bytes, used_bytes);
}

bool config_partition_check_quota(size_t bytes_to_write) {
    if (!s_mounted) {
        return false;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(CONFIG_PARTITION_LABEL, &total, &used) != ESP_OK) {
        return false;
    }

    size_t quota = (total * CONFIG_QUOTA_PERCENT) / 100;
    return (used + bytes_to_write) <= quota;
}

esp_err_t config_partition_sync_to_sd(const char *sd_mirror_path) {
    // TODO: Implement SD mirroring (copy /config to /sdcard/OBD/CFG)
    (void)sd_mirror_path;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t config_partition_restore_from_sd(const char *sd_mirror_path) {
    // TODO: Implement SD restore (copy /sdcard/OBD/CFG to /config)
    (void)sd_mirror_path;
    return ESP_ERR_NOT_SUPPORTED;
}

// Aliases for app_main compatibility
esp_err_t config_part_init(void) { return config_partition_init(); }
esp_err_t config_part_format(void) { return config_partition_format(); }
esp_err_t config_part_restore_from_sd(void) {
    return config_partition_restore_from_sd("/sdcard/OBD/CFG");
}
esp_err_t config_part_mirror_to_sd(void) {
    return config_partition_sync_to_sd("/sdcard/OBD/CFG");
}
