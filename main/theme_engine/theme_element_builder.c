/*
 * theme_element_builder.c — LVGL element construction
 */

#include "theme_element_builder.h"
#include "theme_loader.h"
#include "theme_img_cache.h"
#include "resource_pipeline.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "elem_builder";

/* Build arc element */
static lv_obj_t *build_arc(lv_obj_t *parent, const element_def_t *def) {
    lv_obj_t *arc = lv_arc_create(parent);

    /* Position and size */
    lv_obj_set_pos(arc, def->x, def->y);
    lv_obj_set_size(arc, def->width, def->height);

    /* Arc properties: radius = w/2, range normalized to 0..10000 */
    uint16_t radius = def->width / 2;
    lv_arc_set_bg_angles(arc, def->arc.start_angle, def->arc.end_angle);
    lv_arc_set_range(arc, 0, 10000);
    lv_arc_set_value(arc, 0);

    /* Colors */
    lv_obj_set_style_arc_color(arc, def->color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, def->bg_color, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, def->arc.thickness, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, def->arc.thickness, LV_PART_INDICATOR);

    /* Remove knob */
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    return arc;
}

/* Build bar element */
static lv_obj_t *build_bar(lv_obj_t *parent, const element_def_t *def) {
    /* LVGL lv_bar auto-switches to vertical when h>w, but we need explicit control.
     * Use two rectangles instead for consistent behavior. */

    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_pos(container, def->x, def->y);
    lv_obj_set_size(container, def->width, def->height);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(container, def->bg_color, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);

    /* Indicator rectangle */
    lv_obj_t *indicator = lv_obj_create(container);
    lv_obj_set_style_bg_color(indicator, def->color, 0);
    lv_obj_set_style_border_width(indicator, 0, 0);
    lv_obj_clear_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);

    if (def->bar.vertical) {
        lv_obj_set_size(indicator, def->width, (lv_coord_t)(def->height * def->bar.value));
        lv_obj_align(indicator, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    } else {
        lv_obj_set_size(indicator, (lv_coord_t)(def->width * def->bar.value), def->height);
        lv_obj_align(indicator, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    /* Store indicator as user_data for updates */
    lv_obj_set_user_data(container, indicator);

    return container;
}

/* Build label element */
static lv_obj_t *build_label(lv_obj_t *parent, const element_def_t *def) {
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, def->x, def->y);
    lv_obj_set_size(label, def->width, def->height);

    lv_label_set_text(label, def->label.text);
    lv_obj_set_style_text_color(label, def->color, 0);
    lv_obj_set_style_text_align(label, def->label.align, 0);

    /* Font selection based on size */
    const lv_font_t *font = &lv_font_montserrat_14;
    if (def->label.font_size >= 48) {
        font = &lv_font_montserrat_48;
    } else if (def->label.font_size >= 32) {
        font = &lv_font_montserrat_32;
    } else if (def->label.font_size >= 24) {
        font = &lv_font_montserrat_24;
    } else if (def->label.font_size >= 20) {
        font = &lv_font_montserrat_20;
    } else if (def->label.font_size >= 16) {
        font = &lv_font_montserrat_16;
    }

    lv_obj_set_style_text_font(label, font, 0);

    return label;
}

/* Build image element */
static lv_obj_t *build_image(lv_obj_t *parent, const element_def_t *def) {
    lv_obj_t *img = lv_img_create(parent);

    lv_obj_set_pos(img, def->x, def->y);

    /* Get image from theme */
    const lv_img_dsc_t *img_dsc = theme_loader_get_image(def->image.asset_id);
    if (img_dsc) {
        lv_img_set_src(img, img_dsc);

        /* Set size to image dimensions (no scaling) */
        lv_obj_set_size(img, img_dsc->header.w, img_dsc->header.h);

        /* Pivot point (0,0) per contract */
        lv_img_set_pivot(img, def->image.pivot_x, def->image.pivot_y);

        /* Rotation */
        if (def->image.rotation != 0) {
            lv_img_set_angle(img, def->image.rotation * 10); /* LVGL uses 0.1 deg units */
        }
    } else {
        ESP_LOGW(TAG, "Image asset %u not found", def->image.asset_id);
    }

    return img;
}

/* Build pointer element */
static lv_obj_t *build_pointer(lv_obj_t *parent, const element_def_t *def) {
    lv_obj_t *img = lv_img_create(parent);

    lv_obj_set_pos(img, def->x, def->y);

    /* Get pointer image */
    const lv_img_dsc_t *img_dsc = theme_loader_get_image(def->pointer.asset_id);
    if (img_dsc) {
        /* Apply scale if needed (pre-scale preferred for pointers) */
        if (def->pointer.scale != 1.0f) {
            /* TODO: use pre-scaled cached version */
        }

        lv_img_set_src(img, img_dsc);
        lv_obj_set_size(img, img_dsc->header.w, img_dsc->header.h);

        /* Pivot point */
        lv_img_set_pivot(img, def->pointer.pivot_x, def->pointer.pivot_y);

        /* Initial angle */
        lv_img_set_angle(img, theme_round(def->pointer.angle * 10.0f));
    } else {
        ESP_LOGW(TAG, "Pointer asset %u not found", def->pointer.asset_id);
    }

    return img;
}

lv_obj_t *theme_element_build(lv_obj_t *parent, const element_def_t *def) {
    if (!parent || !def) {
        return NULL;
    }

    switch (def->type) {
        case ELEM_TYPE_ARC:
            return build_arc(parent, def);

        case ELEM_TYPE_BAR:
            return build_bar(parent, def);

        case ELEM_TYPE_LABEL:
            return build_label(parent, def);

        case ELEM_TYPE_IMAGE:
            return build_image(parent, def);

        case ELEM_TYPE_POINTER:
            return build_pointer(parent, def);

        default:
            ESP_LOGE(TAG, "Unknown element type: %d", def->type);
            return NULL;
    }
}

esp_err_t theme_element_update(lv_obj_t *obj, const element_def_t *def, float value) {
    if (!obj || !def) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (def->type) {
        case ELEM_TYPE_ARC: {
            /* Update arc value (0..10000 range) */
            int32_t arc_val = theme_round(value * 10000.0f);
            if (arc_val < 0) arc_val = 0;
            if (arc_val > 10000) arc_val = 10000;
            lv_arc_set_value(obj, arc_val);
            break;
        }

        case ELEM_TYPE_BAR: {
            /* Update bar indicator size */
            lv_obj_t *indicator = (lv_obj_t *)lv_obj_get_user_data(obj);
            if (indicator) {
                if (def->bar.vertical) {
                    lv_coord_t h = theme_round(def->height * value);
                    lv_obj_set_height(indicator, h);
                } else {
                    lv_coord_t w = theme_round(def->width * value);
                    lv_obj_set_width(indicator, w);
                }
            }
            break;
        }

        case ELEM_TYPE_LABEL: {
            /* Format label text */
            char text[128];
            if (strlen(def->label.format) > 0) {
                /* Use formatIntPrintf logic from tr_format.c */
                snprintf(text, sizeof(text), def->label.format, (int)value);
            } else {
                snprintf(text, sizeof(text), "%.0f", value);
            }
            lv_label_set_text(obj, text);
            break;
        }

        case ELEM_TYPE_POINTER: {
            /* Update pointer angle */
            int16_t angle = theme_round(value * 10.0f);  /* value in degrees, LVGL uses 0.1 deg */
            lv_img_set_angle(obj, angle);
            break;
        }

        default:
            break;
    }

    return ESP_OK;
}

esp_err_t theme_element_apply_rule_action(lv_obj_t *obj, element_type_t type,
                                            const char *action, const char *value) {
    if (!obj || !action || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse color if needed */
    lv_color_t color = lv_color_black();
    if (strstr(action, "color") || strstr(action, "Color")) {
        /* Parse hex color (supports 0x, #, or bare 6-digit hex) */
        const char *hex = value;
        if (hex[0] == '0' && hex[1] == 'x') hex += 2;
        else if (hex[0] == '#') hex += 1;

        uint32_t rgb = strtoul(hex, NULL, 16);
        color = lv_color_make((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    }

    /* Apply action based on type */
    if (strcmp(action, "color") == 0) {
        if (type == ELEM_TYPE_ARC) {
            lv_obj_set_style_arc_color(obj, color, LV_PART_INDICATOR);
        } else if (type == ELEM_TYPE_BAR) {
            lv_obj_t *indicator = (lv_obj_t *)lv_obj_get_user_data(obj);
            if (indicator) {
                lv_obj_set_style_bg_color(indicator, color, 0);
            }
        } else if (type == ELEM_TYPE_LABEL) {
            lv_obj_set_style_text_color(obj, color, 0);
        } else if (type == ELEM_TYPE_IMAGE || type == ELEM_TYPE_POINTER) {
            lv_obj_set_style_img_recolor(obj, color, 0);
            lv_obj_set_style_img_recolor_opa(obj, 191, 0);  /* 75% tint per editor */
        }
    } else if (strcmp(action, "bgColor") == 0) {
        if (type == ELEM_TYPE_ARC) {
            lv_obj_set_style_arc_color(obj, color, LV_PART_MAIN);
        } else if (type == ELEM_TYPE_BAR) {
            lv_obj_set_style_bg_color(obj, color, 0);
        }
    } else if (strcmp(action, "text") == 0) {
        if (type == ELEM_TYPE_LABEL) {
            lv_label_set_text(obj, value);
        }
    } else if (strcmp(action, "visible") == 0) {
        bool visible = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        if (visible) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    return ESP_OK;
}
