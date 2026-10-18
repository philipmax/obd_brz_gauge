// inbox_import.c — migrate legacy Flash theme_0 / bootmedia into the SD library.
#include "app_obd_dsp/inbox_import.h"

#include <stdio.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "bsp_obd_dsp/nvs_app.h"
#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/boot_media_mount.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/theme_library.h"
#include "app_obd_dsp/theme_mount.h"

static const char *TAG = "inbox";

/* Pre-refactor bootmedia slot (0xA20000 + 0x5E0000 = end of the 16 MB flash). */
#define LEGACY_BOOTMEDIA_SIZE  0x5E0000u
#define PATH_LEN               128

static const esp_partition_t *bootmedia_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bootmedia");
}

/* ------------------------------------------------------------------ */

static esp_err_t import_theme(void)
{
    if (theme_library_resume_pending() || theme_library_get_state(NULL) == THEME_LIB_WRITING) {
        ESP_LOGI(TAG, "theme_0: library write pending, skipped");
        return ESP_OK;
    }
    uint32_t addr = theme_get_partition_addr();
    if (!addr) return ESP_ERR_NOT_FOUND;

    char cur[STORAGE_THEME_KEY_LEN];
    theme_library_current_key(cur, sizeof(cur));
    uint32_t ledger = nvs_app_imp_theme_crc_get();

    storage_theme_meta_t meta;
    esp_err_t err = storage_theme_meta_from_flash(&meta);
    if (err == ESP_ERR_NOT_FOUND) {
        /* Empty legacy Flash has no effect on the SD selection. */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "theme_0: unusable manifest (%s), not imported", esp_err_to_name(err));
        return ESP_OK;
    }

    uint32_t crc = 0;
    err = storage_flash_crc32(addr, meta.used_len, &crc);
    if (err != ESP_OK) return err;
    if (crc == ledger) return ESP_OK;

    /* Import a legacy package without replacing SD resources or selection. */
    char key[STORAGE_THEME_KEY_LEN];
    storage_theme_key(meta.id, key, sizeof(key));
    if (!sd_card_is_mounted()) {
        ESP_LOGI(TAG, "theme_0: new theme '%s', SD copy deferred (no card)", meta.id);
        return ESP_ERR_INVALID_STATE;
    }

    char dst[PATH_LEN];
    snprintf(dst, sizeof(dst), SD_DIR_THEMES "/%s/" STORAGE_THEME_FILE, key);
    if (storage_sd_file_size(dst) >= 0) {
        if (!cur[0]) {
            err = theme_library_select(key);
            if (err != ESP_OK) return err;
        }
        nvs_app_imp_theme_crc_set(crc);
        return ESP_OK;
    }
    uint32_t wcrc = 0;
    err = storage_copy_flash_to_sd(addr, meta.used_len, dst, &wcrc);
    if (err == ESP_OK && wcrc != crc) err = ESP_ERR_INVALID_CRC;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "theme_0: copy to %s failed: %s", dst, esp_err_to_name(err));
        return err;
    }
    if (!cur[0]) {
        err = theme_library_select(key);  // only after the durable SD copy
        if (err != ESP_OK) return err;
    }
    nvs_app_imp_theme_crc_set(crc);
    ESP_LOGI(TAG, "theme_0: '%s' (%lu bytes, crc %08lx) -> %s", meta.id,
             (unsigned long)meta.used_len, (unsigned long)crc, dst);
    return ESP_OK;
}

static esp_err_t import_bootmedia(void)
{
    const esp_partition_t *p = bootmedia_part();
    if (!p) return ESP_ERR_NOT_FOUND;
    /* Legacy Flash is only a migration source; playback requires SD. */
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;

    uint32_t len = 0, crc = 0;
    esp_err_t err = storage_bootmedia_probe(p->address, p->size, &len, &crc);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;   /* no animation */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bootmedia: unusable image (%s), not imported", esp_err_to_name(err));
        return ESP_OK;
    }
    if (crc == nvs_app_imp_bootmedia_crc_get()) return ESP_OK;

    if (storage_sd_file_size(SD_BOOT_LEGACY) >= 0) {
        nvs_app_imp_bootmedia_crc_set(crc);
        return ESP_OK;  // preserve an animation already on SD
    }
    err = storage_copy_flash_to_sd(p->address, len, SD_BOOT_LEGACY, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bootmedia: copy failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_app_imp_bootmedia_crc_set(crc);
    ESP_LOGI(TAG, "bootmedia: %lu bytes (crc %08lx) -> " SD_BOOT_LEGACY,
             (unsigned long)len, (unsigned long)crc);
    return ESP_OK;
}

esp_err_t inbox_import_run(void)
{
    if (ota_wifi_server_is_active()) return ESP_ERR_INVALID_STATE;
    if (!storage_job_lock(pdMS_TO_TICKS(100))) return ESP_ERR_INVALID_STATE;
    esp_err_t e1 = import_theme();
    esp_err_t e2 = import_bootmedia();
    storage_job_unlock();
    if (e1 == ESP_ERR_INVALID_STATE || e2 == ESP_ERR_INVALID_STATE) return ESP_ERR_INVALID_STATE;
    return e1 != ESP_OK ? e1 : e2;
}

/* ------------------------------------------------------------------ */

esp_err_t inbox_import_bootmedia_rescue(bool *hit_coredump)
{
    if (hit_coredump) *hit_coredump = false;
    const esp_partition_t *p = bootmedia_part();
    if (!p) return ESP_ERR_NOT_FOUND;

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size <= p->address) {
        flash_size = p->address + p->size;
    }
    uint32_t avail = flash_size - p->address;
    if (avail > LEGACY_BOOTMEDIA_SIZE) avail = LEGACY_BOOTMEDIA_SIZE;
    if (avail <= p->size) return ESP_OK;

    uint32_t len = 0, crc = 0;
    esp_err_t err = storage_bootmedia_probe(p->address, avail, &len, &crc);
    if (err != ESP_OK || len <= p->size) return ESP_OK;   /* none, broken, or fits */

    const esp_partition_t *cd = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (hit_coredump && cd && p->address + len > cd->address) *hit_coredump = true;

    ESP_LOGW(TAG, "bootmedia: image of %lu bytes overruns the %lu byte partition",
             (unsigned long)len, (unsigned long)p->size);
    if (!sd_card_is_mounted()) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!storage_job_lock(portMAX_DELAY)) {
        err = ESP_ERR_TIMEOUT;
    } else {
        if (storage_sd_file_size(SD_BOOT_LEGACY) < 0) {
            err = storage_copy_flash_to_sd(p->address, len, SD_BOOT_LEGACY, NULL);
        }
        if (err == ESP_OK) err = nvs_app_imp_bootmedia_crc_set(crc);
        storage_job_unlock();
    }
    /* The tail is erased/reformatted next and the flash copy can never play
     * from the smaller partition: drop it either way (the SD copy plays). */
    boot_media_invalidate_manifest();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bootmedia: rescued to " SD_BOOT_LEGACY);
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bootmedia: no SD card, the oversized boot animation is lost");
    } else {
        ESP_LOGE(TAG, "bootmedia: rescue copy failed (%s), boot animation lost", esp_err_to_name(err));
    }
    return err;
}
