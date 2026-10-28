// boot_media_mount.c — legacy Flash migration access; runtime media lives on SD.
#include "app_obd_dsp/boot_media_mount.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "app_obd_dsp/flash_writer.h"

static const char *TAG = "boot_media";

#define BOOTMEDIA_PARTITION_LABEL "bootmedia"
#define BOOTMEDIA_MANIFEST_OFFSET 0x0000u

static const esp_partition_t *s_part = NULL;
static bool s_missing_logged = false;

static const esp_partition_t *get_partition(void)
{
    if (s_part) return s_part;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      BOOTMEDIA_PARTITION_LABEL);
    if (!s_part && !s_missing_logged) {
        s_missing_logged = true;
        ESP_LOGW(TAG, "bootmedia partition not found");
    }
    if (s_part && s_part->size <= BOOT_MEDIA_BIN_OFFSET) {
        ESP_LOGE(TAG, "bootmedia partition too small (%lu B)", (unsigned long)s_part->size);
        s_part = NULL;
    }
    return s_part;
}

bool boot_media_invalidate_manifest(void)
{
    const esp_partition_t *p = get_partition();
    if (!p) return false;
    // Migration invalidates an oversized legacy image before its tail is
    // erased by config/coredump recovery; runtime uploads never write here.
    esp_err_t err = flash_writer_erase(p, BOOTMEDIA_MANIFEST_OFFSET, BOOT_MEDIA_MANIFEST_MAX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "manifest erase failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
