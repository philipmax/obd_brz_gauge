#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * flash_writer - partition erase helper for legacy media recovery.
 * Runtime media uses SD.
 * -------------------------------------------------------------------------- */

/* Erase [off, off+len) rounded out to 4 KB sectors (clamped to the partition). */
esp_err_t flash_writer_erase(const esp_partition_t *part, uint32_t off, size_t len);

#ifdef __cplusplus
}
#endif
