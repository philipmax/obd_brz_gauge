#pragma once
// flash_partition_utils.h — PSRAM-safe helper for writing to flash partitions.
//
// The ESP32-S3 SDMMC DMA engine cannot read from PSRAM, and esp_partition_write
// (which disables the flash cache) requires data in internal SRAM on some IDF
// revisions. This helper copies arbitrary source buffers (which may live in
// PSRAM) through a dynamically-allocated internal-SRAM staging chunk before
// each partition write, avoiding the need for large static internal-RAM arrays.

#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

/* Staging chunk size: 16 KB keeps stack use low and gives reasonable throughput. */
#define FLASH_WRITE_CHUNK  (16 * 1024)

/**
 * @brief  Write `len` bytes from `src` into `part` at `part_offset`.
 *
 * Allocates a FLASH_WRITE_CHUNK byte internal-SRAM staging buffer once, copies
 * data in chunks, then frees it. Safe to call with src in PSRAM.
 *
 * @return ESP_OK on success, or the first error encountered.
 */
static inline esp_err_t flash_part_write_safe(const esp_partition_t *part,
                                               uint32_t part_offset,
                                               const uint8_t *src,
                                               size_t len)
{
    if (!part || (!src && len > 0)) return ESP_ERR_INVALID_ARG;
    if ((uint64_t)part_offset + len > part->size) return ESP_ERR_INVALID_SIZE;
    if (len == 0) return ESP_OK;

    uint8_t *staging = (uint8_t *)malloc(FLASH_WRITE_CHUNK);
    if (!staging) {
        ESP_LOGE("flash_utils", "staging alloc failed");
        return ESP_ERR_NO_MEM;
    }

    size_t done = 0;
    esp_err_t err = ESP_OK;

    while (done < len) {
        size_t chunk = len - done;
        if (chunk > FLASH_WRITE_CHUNK) chunk = FLASH_WRITE_CHUNK;
        memcpy(staging, src + done, chunk);
        err = esp_partition_write(part, part_offset + done, staging, chunk);
        if (err != ESP_OK) {
            ESP_LOGE("flash_utils", "write failed at offset %u: %s",
                     (unsigned)(part_offset + done), esp_err_to_name(err));
            break;
        }
        done += chunk;
    }

    free(staging);
    return err;
}
