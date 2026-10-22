#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Raw access to the `bootmedia` partition (found by label, 0x4C0000 bytes).
 *
 * Legacy layout: [manifest padded to 4 KB][delta_varint stream].
 * Runtime uploads/playback use SD. The only retained Flash mutation is
 * invalidating an oversized legacy image during config/coredump rescue.
 * -------------------------------------------------------------------------- */

#define BOOT_MEDIA_MANIFEST_MAX  0x1000u
#define BOOT_MEDIA_BIN_OFFSET    0x1000u

/* Locate the partition; false when this layout has no bootmedia partition. */
bool   boot_media_mount(void);
size_t boot_media_partition_size(void);
/* Free bytes for the bin region (partition size - 4 KB), 0 if not mounted. */
size_t boot_media_get_free_space(void);
/* Manifest slot holds a plausible manifest (first byte a-z). */
bool   boot_media_has_block_video(void);

/* Erase only the 4 KB manifest sector (fast invalidate). */
bool boot_media_invalidate_manifest(void);
size_t    boot_media_raw_manifest_max(void);
/* Read the manifest slot (size <= 4 KB). */
esp_err_t boot_media_raw_read_manifest(uint8_t *buf, size_t size);
/* Read len bytes of the bin region at bin_off (relative to 0x1000) into buf
 * (internal RAM or PSRAM). Used for chunked 16 KB streaming. */
esp_err_t boot_media_raw_read(uint32_t bin_off, void *buf, size_t len);
/* Bytes available in the bin region. */
size_t    boot_media_raw_bin_capacity(void);

#ifdef __cplusplus
}
#endif
