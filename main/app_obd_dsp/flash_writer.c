#include "flash_writer.h"

#include <stdint.h>
#include "esp_log.h"

static const char *TAG = "flash_writer";

#define SECTOR 0x1000u

esp_err_t flash_writer_erase(const esp_partition_t *part, uint32_t off, size_t len)
{
    if (!part) return ESP_ERR_INVALID_ARG;
    if (len == 0) return ESP_OK;
    uint32_t start = off & ~(SECTOR - 1);
    uint64_t end = ((uint64_t)off + len + SECTOR - 1) & ~(uint64_t)(SECTOR - 1);
    if (end > part->size) end = part->size;
    if (start >= end) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = esp_partition_erase_range(part, start, (size_t)(end - start));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s erase 0x%x+0x%x: %s", part->label, (unsigned)start,
                 (unsigned)(end - start), esp_err_to_name(err));
    }
    return err;
}
