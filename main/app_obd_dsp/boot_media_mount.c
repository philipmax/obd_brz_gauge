// boot_media_mount.c — legacy Flash migration access; runtime media lives on SD.
#include "app_obd_dsp/boot_media_mount.h"

#include <string.h>
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

bool boot_media_mount(void)
{
    return get_partition() != NULL;
}

size_t boot_media_partition_size(void)
{
    const esp_partition_t *p = get_partition();
    return p ? p->size : 0;
}

size_t boot_media_get_free_space(void)
{
    const esp_partition_t *p = get_partition();
    return p ? p->size - BOOT_MEDIA_BIN_OFFSET : 0;
}

size_t boot_media_raw_bin_capacity(void)
{
    return boot_media_get_free_space();
}

size_t boot_media_raw_manifest_max(void)
{
    return BOOT_MEDIA_MANIFEST_MAX;
}

bool boot_media_has_block_video(void)
{
    const esp_partition_t *p = get_partition();
    if (!p) return false;
    uint8_t probe = 0xFF;
    if (esp_partition_read(p, BOOTMEDIA_MANIFEST_OFFSET, &probe, 1) != ESP_OK) return false;
    // A manifest starts with a lowercase key ("canvas_width="); 0xFF = erased.
    return probe >= 'a' && probe <= 'z';
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

esp_err_t boot_media_raw_read_manifest(uint8_t *buf, size_t size)
{
    const esp_partition_t *p = get_partition();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (!buf || size == 0) return ESP_ERR_INVALID_ARG;
    if (size > BOOT_MEDIA_MANIFEST_MAX) size = BOOT_MEDIA_MANIFEST_MAX;
    return esp_partition_read(p, BOOTMEDIA_MANIFEST_OFFSET, buf, size);
}

esp_err_t boot_media_raw_read(uint32_t bin_off, void *buf, size_t len)
{
    const esp_partition_t *p = get_partition();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (!buf) return ESP_ERR_INVALID_ARG;
    size_t cap = p->size - BOOT_MEDIA_BIN_OFFSET;
    if (bin_off > cap || len > cap - bin_off) return ESP_ERR_INVALID_SIZE;
    if (len == 0) return ESP_OK;
    // esp_partition_read goes through the flash cache-safe path and accepts
    // PSRAM destinations (it bounces internally when needed).
    return esp_partition_read(p, BOOT_MEDIA_BIN_OFFSET + bin_off, buf, len);
}
