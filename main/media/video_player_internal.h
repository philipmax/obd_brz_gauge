#pragma once
/*
 * video_player_internal.h — media-internal session queries used by
 * boot_video and video_elem to tell their own session from someone else's
 * (video_player has a single session; start() replaces it). LVGL task only.
 */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* video_view of the current COMPOSITE session, NULL when idle or fullscreen. */
lv_obj_t *video_player_current_view(void);
/* Nonzero identity of the current session; 0 when idle. Changes on start(). */
uint32_t  video_player_session_id(void);
/* A FULLSCREEN session exists (playing, or ended but not stopped yet). */
bool      video_player_fullscreen_active(void);
/* First frame presented; FULLSCREEN includes completion of its final DMA strip. */
bool      video_player_first_frame_presented(void);

#ifdef __cplusplus
}
#endif
