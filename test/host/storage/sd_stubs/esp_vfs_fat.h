#pragma once
#include <stdbool.h>
#include "driver/sdmmc_host.h"
typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_sdmmc_mount_config_t;
esp_err_t esp_vfs_fat_sdmmc_mount(const char *path, const sdmmc_host_t *host,
    const sdmmc_slot_config_t *slot, const esp_vfs_fat_sdmmc_mount_config_t *config,
    sdmmc_card_t **card);
esp_err_t esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *card);
esp_err_t esp_vfs_fat_info(const char *path, uint64_t *total, uint64_t *free_bytes);
