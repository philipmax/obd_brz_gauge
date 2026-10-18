#include "flash_writer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"

static const char *TAG = "flash_writer";

#define SECTOR 0x1000u

esp_err_t flash_writer_write(const esp_partition_t *part, uint32_t off,
                             const void *src, size_t len)
{
    if (!part || (!src && len > 0)) return ESP_ERR_INVALID_ARG;
    if ((uint64_t)off + len > part->size) return ESP_ERR_INVALID_SIZE;
    if (len == 0) return ESP_OK;

    const uintptr_t src_addr = (uintptr_t)src;
    const bool range_addr_valid = len - 1 <= UINTPTR_MAX - src_addr;
    const void *last = range_addr_valid ? (const void *)(src_addr + len - 1) : NULL;
    if (range_addr_valid && esp_ptr_internal(src) && esp_ptr_internal(last)) {
        // Partition writes require an internal-RAM source. Avoid an allocation
        // and memcpy when the complete input span is already in internal RAM.
        esp_err_t err = esp_partition_write(part, off, src, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s write @0x%x: %s", part->label, (unsigned)off,
                     esp_err_to_name(err));
        }
        return err;
    }

    size_t chunk_max = len < FLASH_WRITER_CHUNK ? len : FLASH_WRITER_CHUNK;
    uint8_t *staging = heap_caps_malloc(chunk_max, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!staging) {
        ESP_LOGE(TAG, "staging alloc failed (%u B)", (unsigned)chunk_max);
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *p = src;
    size_t done = 0;
    esp_err_t err = ESP_OK;
    while (done < len) {
        size_t n = len - done;
        if (n > chunk_max) n = chunk_max;
        memcpy(staging, p + done, n);
        err = esp_partition_write(part, off + done, staging, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s write @0x%x: %s", part->label, (unsigned)(off + done),
                     esp_err_to_name(err));
            break;
        }
        done += n;
    }
    heap_caps_free(staging);
    return err;
}

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
