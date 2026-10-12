/*
 * SD Import Scanner
 * Processes files in /OBD/IMPORT directory
 */

#include "sd_import.h"
#include "bsp_obd_dsp/sd_card.h"
#include "esp_log.h"
#include <dirent.h>
#include <string.h>

static const char *TAG = "sd_import";

esp_err_t sd_import_scan(void) {
    if (!sd_card_is_mounted()) {
        ESP_LOGD(TAG, "SD card not mounted, skipping import scan");
        return ESP_ERR_INVALID_STATE;
    }

    const char *import_dir = "/sdcard/OBD/IMPORT";
    DIR *dir = opendir(import_dir);
    if (!dir) {
        ESP_LOGD(TAG, "Import directory not found");
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry;
    int vehicle_count = 0;
    int sidecar_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;

        const char *name = entry->d_name;
        size_t len = strlen(name);

        // Check for .vehicle.json files
        if (len > 13 && strcmp(name + len - 13, ".vehicle.json") == 0) {
            ESP_LOGI(TAG, "Found vehicle profile: %s", name);
            // TODO: Parse and import vehicle profile
            vehicle_count++;
        }
        // Check for .gaugeproj.json files
        else if (len > 15 && strcmp(name + len - 15, ".gaugeproj.json") == 0) {
            ESP_LOGI(TAG, "Found gaugeproj sidecar: %s", name);
            // TODO: Parse and import sidecar
            sidecar_count++;
        }
        // Check for ACTIVATE.TXT
        else if (strcmp(name, "ACTIVATE.TXT") == 0) {
            ESP_LOGI(TAG, "Found ACTIVATE.TXT");
            // TODO: Read and activate specified profile
        }
        // Check for RESET.TXT
        else if (strcmp(name, "RESET.TXT") == 0) {
            ESP_LOGI(TAG, "Found RESET.TXT");
            // TODO: Reset to generic profile
        }
    }

    closedir(dir);

    if (vehicle_count > 0 || sidecar_count > 0) {
        ESP_LOGI(TAG, "Import scan complete: %d vehicles, %d sidecars",
                 vehicle_count, sidecar_count);
    }

    return ESP_OK;
}
