#pragma once
/*
 * video_view.h — Custom LVGL widget for video rendering
 *
 * Replaces lv_img for video frames to correctly report coverage.
 * LVGL 8.4's lv_img never reports "covered" (lv_obj.c:511-513, lv_img.c:548),
 * causing all layers below video to redraw every frame.
 *
 * video_view is a plain lv_obj with custom DRAW_MAIN and COVER_CHECK callbacks.
 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create video_view widget.
 * Parent must be a valid LVGL object.
 */
lv_obj_t *video_view_create(lv_obj_t *parent);

/**
 * Set current frame data.
 * Data must be RGB565, size must match widget dimensions.
 * Does NOT copy data - caller must keep it valid until next frame.
 */
void video_view_set_frame(lv_obj_t *obj, const uint16_t *rgb565_data,
                          uint16_t width, uint16_t height);
void video_view_set_frame_alpha(lv_obj_t *obj, const uint8_t *rgb565a_data,
                                uint16_t width, uint16_t height);

/**
 * Clear frame (show black or placeholder).
 */
void video_view_clear(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
