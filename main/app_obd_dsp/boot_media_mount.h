#pragma once

#include <stdbool.h>

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

/* Erase only the 4 KB manifest sector (fast invalidate). */
bool boot_media_invalidate_manifest(void);

#ifdef __cplusplus
}
#endif
