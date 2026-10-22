#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Read-only legacy theme_0 access for migration to SD.

// Find the theme_0 partition; returns true on success.
bool theme_mount(void);

// Get theme_0's total size in bytes; returns 0 if not mounted.
size_t theme_get_partition_size(void);

// Absolute flash address of theme_0 (for mmap reads); 0 if not found.
uint32_t theme_get_partition_addr(void);

// Read `len` bytes at `offset` from theme_0 into `buf` (any RAM).
esp_err_t theme_raw_read(uint32_t offset, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
