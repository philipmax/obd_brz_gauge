/*
 * video_view.c — Custom LVGL widget for video rendering
 */

#include "video_view.h"
#include "esp_log.h"

static const char *TAG = "video_view";

/* Widget data */
typedef struct {
    const uint16_t *frame_data;
    uint16_t width;
    uint16_t height;
} video_view_data_t;

/* Draw callback */
static void video_view_draw_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);

    video_view_data_t *data = (video_view_data_t *)lv_obj_get_user_data(obj);
    if (!data || !data->frame_data) {
        return;
    }

    /* Get object coordinates */
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    /* Create image descriptor */
    lv_img_dsc_t img_dsc;
    img_dsc.header.always_zero = 0;
    img_dsc.header.w = data->width;
    img_dsc.header.h = data->height;
    img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    img_dsc.data_size = data->width * data->height * 2;
    img_dsc.data = (const uint8_t *)data->frame_data;

    /* Draw image */
    lv_draw_img_dsc_t img_draw_dsc;
    lv_draw_img_dsc_init(&img_draw_dsc);

    lv_draw_img(draw_ctx, &img_draw_dsc, &coords, &img_dsc);
}

/* Cover check callback - reports widget as covering its area */
static void video_view_cover_check_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_COVER_CHECK) {
        lv_cover_check_info_t *info = lv_event_get_param(e);

        /* Check if we have frame data */
        lv_obj_t *obj = lv_event_get_target(e);
        video_view_data_t *data = (video_view_data_t *)lv_obj_get_user_data(obj);

        if (data && data->frame_data) {
            /* Report as fully covered (opaque) */
            if (info->res == LV_COVER_RES_NOT_COVER) {
                info->res = LV_COVER_RES_COVER;
            }
        }
    }
}

lv_obj_t *video_view_create(lv_obj_t *parent) {
    if (!parent) {
        return NULL;
    }

    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) {
        return NULL;
    }

    /* Allocate widget data */
    video_view_data_t *data = lv_mem_alloc(sizeof(video_view_data_t));
    if (!data) {
        lv_obj_del(obj);
        return NULL;
    }

    data->frame_data = NULL;
    data->width = 0;
    data->height = 0;

    lv_obj_set_user_data(obj, data);

    /* Configure object */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);

    /* Register callbacks */
    lv_obj_add_event_cb(obj, video_view_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, video_view_cover_check_cb, LV_EVENT_COVER_CHECK, NULL);

    ESP_LOGD(TAG, "video_view created");
    return obj;
}

void video_view_set_frame(lv_obj_t *obj, const uint16_t *rgb565_data,
                          uint16_t width, uint16_t height) {
    if (!obj) {
        return;
    }

    video_view_data_t *data = (video_view_data_t *)lv_obj_get_user_data(obj);
    if (!data) {
        return;
    }

    data->frame_data = rgb565_data;
    data->width = width;
    data->height = height;

    /* Trigger redraw */
    lv_obj_invalidate(obj);
}

void video_view_clear(lv_obj_t *obj) {
    if (!obj) {
        return;
    }

    video_view_data_t *data = (video_view_data_t *)lv_obj_get_user_data(obj);
    if (!data) {
        return;
    }

    data->frame_data = NULL;
    lv_obj_invalidate(obj);
}
