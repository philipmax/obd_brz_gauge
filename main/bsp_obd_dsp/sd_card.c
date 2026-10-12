/*
 * SD Card Driver for ESP32-S3 (SDMMC 1-bit mode)
 * Hardware: CLK=GPIO14, CMD=GPIO17, D0=GPIO16
 * 32KB cluster size required for optimal performance
 */

#include "sd_card.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <string.h>

static const char *TAG = "sd_card";

#define SD_MOUNT_POINT "/sdcard"
#define SD_MAX_FILES 5

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static uint8_t *s_dma_buffer = NULL;  // 16KB internal DMA buffer

esp_err_t sd_card_init(void) {
    if (s_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    // Allocate 16KB internal RAM DMA buffer (SDMMC can't DMA to PSRAM on S3)
    s_dma_buffer = heap_caps_malloc(16384, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buffer) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,  // Never auto-format
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = 32 * 1024  // 32KB cluster
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;  // 1-bit mode
    host.max_freq_khz = 40000;  // Try 40MHz first, fallback to 20MHz

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = GPIO_NUM_14;
    slot_config.cmd = GPIO_NUM_17;
    slot_config.d0 = GPIO_NUM_16;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);

    if (ret == ESP_OK) {
        s_mounted = true;
        sdmmc_card_print_info(stdout, s_card);
        ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);

        // Create /OBD directory structure
        sd_card_ensure_directories();
        return ESP_OK;
    }

    // Fallback to 20MHz if 40MHz failed
    if (ret != ESP_OK && host.max_freq_khz == 40000) {
        ESP_LOGW(TAG, "40MHz failed, trying 20MHz");
        host.max_freq_khz = 20000;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
        if (ret == ESP_OK) {
            s_mounted = true;
            ESP_LOGI(TAG, "SD card mounted at %s (20MHz)", SD_MOUNT_POINT);
            sd_card_ensure_directories();
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    if (s_dma_buffer) {
        free(s_dma_buffer);
        s_dma_buffer = NULL;
    }
    return ret;
}

esp_err_t sd_card_deinit(void) {
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (ret == ESP_OK) {
        s_mounted = false;
        s_card = NULL;
        if (s_dma_buffer) {
            free(s_dma_buffer);
            s_dma_buffer = NULL;
        }
        ESP_LOGI(TAG, "SD card unmounted");
    }
    return ret;
}

bool sd_card_is_mounted(void) {
    return s_mounted;
}

const char *sd_card_get_mount_point(void) {
    return SD_MOUNT_POINT;
}

esp_err_t sd_card_ensure_directories(void) {
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *dirs[] = {
        SD_MOUNT_POINT "/OBD",
        SD_MOUNT_POINT "/OBD/IMPORT",
        SD_MOUNT_POINT "/OBD/THEMES",
        SD_MOUNT_POINT "/OBD/VIDEO",
        SD_MOUNT_POINT "/OBD/BOOT",
        SD_MOUNT_POINT "/OBD/CFG",
        SD_MOUNT_POINT "/OBD/CATALOG",
        SD_MOUNT_POINT "/OBD/EXPORT"
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

uint64_t sd_card_get_total_bytes(void) {
    if (!s_mounted || !s_card) {
        return 0;
    }
    return ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size;
}

uint64_t sd_card_get_free_bytes(void) {
    if (!s_mounted) {
        return 0;
    }

    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) {
        return 0;
    }

    uint64_t free_bytes = ((uint64_t)free_clusters) * fs->csize * 512;
    return free_bytes;
}
