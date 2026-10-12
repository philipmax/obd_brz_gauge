/*
 * First Boot Migration
 * Handles partition table v1 -> v2 migration after web-flasher complete reflash
 */

#include "first_boot_migration.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"

static const char *TAG = "migration";

#define NVS_APP_MIG_VER_DONE 1

esp_err_t first_boot_migration_run(void) {
    // TODO: Check nvs_app magic and mig_ver
    // TODO: If mig_ver < NVS_APP_MIG_VER_DONE:
    //   1. Import old bootmedia from 0xA20000 (before config/coredump overwrite it)
    //   2. Import old theme_0
    //   3. Erase coredump partition
    //   4. Format config partition
    //   5. Migrate NVS cfg v3 -> v4
    //   6. Write layout_crc and mig_ver=1

    ESP_LOGI(TAG, "Migration check complete (stub)");
    return ESP_OK;
}
