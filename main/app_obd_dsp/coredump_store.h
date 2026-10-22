#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * coredump_store - access to the `coredump` partition (ELF + CRC32 format).
 * Served by GET /ota/coredump and POST /ota/coredump/erase.
 * -------------------------------------------------------------------------- */

/* Log a one-line summary of a stored dump at boot (task, PC, reason). */
void      coredump_store_boot_check(void);
/* True when a valid dump is stored; *size = image size in bytes. */
bool      coredump_store_present(size_t *size);
/* Read len bytes of the image at off. */
esp_err_t coredump_store_read(uint32_t off, void *buf, size_t len);
esp_err_t coredump_store_erase(void);

#ifdef __cplusplus
}
#endif
