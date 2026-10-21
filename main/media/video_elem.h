#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * video_elem - theme compose-mode video (image elements named vid_* / video_*).
 *
 * Looks up SD_DIR_THEMES/<theme_key>/<key>.obv, then SD_DIR_VIDEO/<key>.obv.
 * The element is a video_view object (reports COVER correctly) placed over
 * the poster image; when no file / no card / decode failure it hides itself
 * and the poster stays visible. Only one video decodes at a time; creating a
 * second one pauses the first.
 *
 * All calls are made from the LVGL task. Deleting the object (or its screen)
 * stops playback and frees its buffers.
 * -------------------------------------------------------------------------- */

/* Returns NULL when the file is missing; key is the element id without the
 * vid_/video_ prefix. */
lv_obj_t *video_elem_create(lv_obj_t *parent, const char *theme_key, const char *key,
                            lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
/* Start directly in numeric mode at the first frame, without a timed session. */
lv_obj_t *video_elem_create_value(lv_obj_t *parent, const char *theme_key, const char *key,
                                  lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);
/* Enable numeric control and remember position across ownership / asset changes. */
void      video_elem_set_position(lv_obj_t *obj, double position);
/* Rule `visible`: hidden == paused. */
void      video_elem_set_paused(lv_obj_t *obj, bool paused);
/* Set the rendered video opacity. LVGL opacity is 0..255. */
void      video_elem_set_opa(lv_obj_t *obj, lv_opa_t opa);
/* Rule `asset`: switch to another key (fileName from gaugeproj). */
bool      video_elem_set_key(lv_obj_t *obj, const char *key);
/* Stop all players and free ring/frame buffers (entering OTA). */
void      video_elem_release_all(void);

#ifdef __cplusplus
}
#endif
