#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * boot_video - boot animation facade used by ui_ext.c.
 *
 * Startup media is independent of the selected theme. Source priority (first valid):
 *   1. Shared SD boot.obv, then boot.jpg
 *   2. Legacy SD delta_varint bootmedia.raw
 *   3. none                  caller shows the built-in logo
 *
 * All calls are made from the LVGL task.
 * -------------------------------------------------------------------------- */

typedef enum {
    BOOT_VIDEO_SRC_NONE = 0,
    BOOT_VIDEO_SRC_SD_OBV,
    BOOT_VIDEO_SRC_SD_JPEG,
    BOOT_VIDEO_SRC_SD_LEGACY,
} boot_video_src_t;

typedef enum {
    BOOT_VIDEO_RUNNING = 0,
    BOOT_VIDEO_COMPLETED,   /* all frames displayed / still-image duration elapsed */
    BOOT_VIDEO_FAILED,      /* decode error, timeout, external stop or replacement */
} boot_video_state_t;

/* Prepare playback on parent (normally a dedicated black screen). out_obj
 * receives the drawing object (may be NULL for fullscreen OBV, which drives
 * the panel directly). out_src receives the selected source when non-NULL.
 * Returns false when no source is usable. */
bool     boot_video_create(lv_obj_t *parent, lv_obj_t **out_obj, boot_video_src_t *out_src);
/* Advance to elapsed_ms since playback start. Still images remain for 2.5 s. */
void     boot_video_update(uint32_t elapsed_ms);
/* Only COMPLETED is safe to retain while waiting for the theme. */
boot_video_state_t boot_video_get_state(void);
/* True once the first frame is visible (fullscreen playback can initially be loading). */
bool     boot_video_first_frame_presented(void);
bool     boot_video_is_finished(void);
/* Stop and free everything (safe to call when not playing). */
void     boot_video_destroy(void);

#ifdef __cplusplus
}
#endif
