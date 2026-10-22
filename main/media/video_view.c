/*
 * video_view.c — LVGL widget that draws an RGB565 frame and reports COVER.
 *
 * A plain lv_obj with a transparent background; the frame (LV_COLOR_16_SWAP
 * byte order, i.e. what media_jpeg outputs) is drawn centred in the object
 * and clipped to it. COVER is reported only when the frame fills the whole
 * object, the checked area lies inside it and nothing (opa, blend mode,
 * transform, clip_corner) can make it translucent.
 *
 * State lives in the event callback's user data, so obj->user_data stays
 * free for the owner (theme engine).
 */

#include "media/video_view.h"

#include "esp_log.h"

typedef struct {
    const uint8_t *data;
    uint16_t       width;
    uint16_t       height;
} video_view_state_t;

static void view_event_cb(lv_event_t *e);

static video_view_state_t *view_state(lv_obj_t *obj)
{
    return obj ? (video_view_state_t *)lv_obj_get_event_user_data(obj, view_event_cb) : NULL;
}

/* Absolute area of the frame: centred in the object (may exceed it). */
static void frame_area(lv_obj_t *obj, const video_view_state_t *st, lv_area_t *out)
{
    lv_coord_t ow = lv_area_get_width(&obj->coords);
    lv_coord_t oh = lv_area_get_height(&obj->coords);
    out->x1 = obj->coords.x1 + (ow - (lv_coord_t)st->width) / 2;
    out->y1 = obj->coords.y1 + (oh - (lv_coord_t)st->height) / 2;
    out->x2 = out->x1 + st->width - 1;
    out->y2 = out->y1 + st->height - 1;
}

static void view_cover_check(lv_event_t *e, lv_obj_t *obj, const video_view_state_t *st)
{
    lv_cover_check_info_t *info = lv_event_get_param(e);
    if (info->res == LV_COVER_RES_MASKED || !st->data) return;   /* keep the class result */
    if (lv_obj_get_style_clip_corner(obj, LV_PART_MAIN)) return;
    lv_area_t fa;
    frame_area(obj, st, &fa);
    if (!_lv_area_is_in(&obj->coords, &fa, 0)) return;            /* frame smaller than obj */
    if (!_lv_area_is_in(info->area, &obj->coords, 0)) return;     /* area outside the obj */
    if (lv_obj_get_style_opa(obj, LV_PART_MAIN) < LV_OPA_MAX ||
        lv_obj_get_style_img_opa(obj, LV_PART_MAIN) < LV_OPA_MAX ||
        lv_obj_get_style_blend_mode(obj, LV_PART_MAIN) != LV_BLEND_MODE_NORMAL ||
        lv_obj_get_style_transform_zoom(obj, LV_PART_MAIN) != LV_IMG_ZOOM_NONE ||
        lv_obj_get_style_transform_angle(obj, LV_PART_MAIN) != 0) {
        return;
    }
    info->res = LV_COVER_RES_COVER;   /* override the transparent-bg NOT_COVER */
}

static void view_draw(lv_event_t *e, lv_obj_t *obj, const video_view_state_t *st)
{
    if (!st->data) return;
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t fa, clip;
    frame_area(obj, st, &fa);
    if (!_lv_area_intersect(&clip, draw_ctx->clip_area, &obj->coords)) return;
    if (!_lv_area_intersect(&clip, &clip, &fa)) return;

    lv_draw_img_dsc_t dsc;
    lv_draw_img_dsc_init(&dsc);
    dsc.opa = lv_obj_get_style_img_opa(obj, LV_PART_MAIN);
    dsc.blend_mode = lv_obj_get_style_blend_mode(obj, LV_PART_MAIN);

    const lv_area_t *clip_ori = draw_ctx->clip_area;
    draw_ctx->clip_area = &clip;
    /* Decoded draw: no image cache / decoder lookup per frame. */
    lv_draw_img_decoded(draw_ctx, &dsc, &fa, st->data, LV_IMG_CF_TRUE_COLOR);
    draw_ctx->clip_area = clip_ori;
}

static void view_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    video_view_state_t *st = lv_event_get_user_data(e);
    if (!st) return;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_COVER_CHECK:
        view_cover_check(e, obj, st);
        break;
    case LV_EVENT_DRAW_MAIN:
        view_draw(e, obj, st);
        break;
    case LV_EVENT_DELETE:
        /* No event reaches the object after this. Owners that still need
         * the view while it is deleted (video_elem stopping the player)
         * hook DELETE with LV_EVENT_PREPROCESS so they run before this. */
        lv_mem_free(st);
        break;
    default:
        break;
    }
}

lv_obj_t *video_view_create(lv_obj_t *parent)
{
    video_view_state_t *st = lv_mem_alloc(sizeof(*st));
    if (!st) return NULL;
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) {
        lv_mem_free(st);
        return NULL;
    }
    st->data = NULL;
    st->width = 0;
    st->height = 0;
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, view_event_cb, LV_EVENT_ALL, st);
    return obj;
}

void video_view_set_frame(lv_obj_t *obj, const uint16_t *rgb565_data,
                          uint16_t width, uint16_t height)
{
    video_view_state_t *st = view_state(obj);
    if (!st) return;
    if (!rgb565_data || !width || !height) {
        video_view_clear(obj);
        return;
    }
    st->data = (const uint8_t *)rgb565_data;
    st->width = width;
    st->height = height;
    lv_obj_invalidate(obj);
}

void video_view_clear(lv_obj_t *obj)
{
    video_view_state_t *st = view_state(obj);
    if (!st || !st->data) return;
    st->data = NULL;
    st->width = 0;
    st->height = 0;
    lv_obj_invalidate(obj);
}
