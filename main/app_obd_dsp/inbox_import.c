/*
 * Inbox Import
 * Processes theme_0 and bootmedia partitions written by web-flasher or App
 */

#include "inbox_import.h"
#include "esp_log.h"

static const char *TAG = "inbox";

esp_err_t inbox_import_run(void) {
    // TODO: Check theme_0 partition CRC vs ledger in nvs_app
    // TODO: If different, copy to SD /OBD/THEMES/<id>/theme.bin
    // TODO: Check bootmedia partition manifest
    // TODO: If new, copy to SD /OBD/BOOT/

    ESP_LOGI(TAG, "Inbox import complete (stub)");
    return ESP_OK;
}
