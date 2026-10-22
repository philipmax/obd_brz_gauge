#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * flash_writer - shared PSRAM-safe partition access for config/coredump
 * maintenance and legacy recovery. Runtime media uses SD. Internal staging
 * buffers are allocated only for a call and freed afterwards.
 * -------------------------------------------------------------------------- */

#define FLASH_WRITER_CHUNK  (16 * 1024)

/* Write len bytes from src (internal RAM or PSRAM) to part at off. The
 * target range must already be erased. */
esp_err_t flash_writer_write(const esp_partition_t *part, uint32_t off,
                             const void *src, size_t len);
/* Erase [off, off+len) rounded out to 4 KB sectors (clamped to the partition). */
esp_err_t flash_writer_erase(const esp_partition_t *part, uint32_t off, size_t len);

#ifdef __cplusplus
}
#endif
