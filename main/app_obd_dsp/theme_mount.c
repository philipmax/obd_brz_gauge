// theme_mount.c — legacy theme_0 access for migration to SD.
// Runtime theme loading and installation use SD files.
#include "app_obd_dsp/theme_mount.h"

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "theme_mount";

#define THEME_PARTITION_LABEL  "theme_0"

static const esp_partition_t *s_part = NULL;

static const esp_partition_t *get_partition(void)
{
    if (s_part) {
        return s_part;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_ANY,
                                       THEME_PARTITION_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "theme_0 partition not found");
        return NULL;
    }
    return s_part;
}

bool theme_mount(void)
{
    return get_partition() != NULL;
}

size_t theme_get_partition_size(void)
{
    const esp_partition_t *p = get_partition();
    return p ? p->size : 0;
}

uint32_t theme_get_partition_addr(void)
{
    const esp_partition_t *p = get_partition();
    return p ? p->address : 0;
}

esp_err_t theme_raw_read(uint32_t offset, void *buf, size_t len)
{
    const esp_partition_t *p = get_partition();
    if (!p || (!buf && len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint64_t)offset + len > p->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return len ? esp_partition_read(p, offset, buf, len) : ESP_OK;
}
