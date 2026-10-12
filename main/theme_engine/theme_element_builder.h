#pragma once
/*
 * theme_element_builder.h — LVGL element construction from theme data
 *
 * Builds LVGL objects (arc, bar, label, image, pointer) from editor's element definitions.
 * Aligns with editor behavior: rounding R(v)=floor(v+0.5), arc radius=w/2, etc.
 */

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Element types matching editor */
typedef enum {
    ELEM_TYPE_ARC = 0,
    ELEM_TYPE_BAR,
    ELEM_TYPE_LABEL,
    ELEM_TYPE_IMAGE,
    ELEM_TYPE_POINTER,
} element_type_t;

/* Element descriptor (parsed from theme manifest) */
typedef struct {
    uint32_t id;
    element_type_t type;

    /* Common properties */
    int16_t x, y;
    uint16_t width, height;
    lv_color_t color;
    lv_color_t bg_color;

    /* Type-specific properties */
    union {
        struct {  /* Arc */
            int16_t start_angle;
            int16_t end_angle;
            uint16_t thickness;
        } arc;

        struct {  /* Bar */
            float value;  /* 0..1 */
            bool vertical;
        } bar;

        struct {  /* Label */
            char text[128];
            char format[32];
            uint8_t font_size;
            lv_align_t align;
        } label;

        struct {  /* Image */
            uint32_t asset_id;
            int16_t rotation;
            lv_coord_t pivot_x, pivot_y;
        } image;

        struct {  /* Pointer */
            uint32_t asset_id;
            float angle;  /* Degrees */
            lv_coord_t pivot_x, pivot_y;
            float scale;
        } pointer;
    };
} element_def_t;

/**
 * Build LVGL object from element definition.
 * Returns created object, or NULL on error.
 */
lv_obj_t *theme_element_build(lv_obj_t *parent, const element_def_t *def);

/**
 * Update element with new value (for data binding).
 * Only updates properties that changed.
 */
esp_err_t theme_element_update(lv_obj_t *obj, const element_def_t *def, float value);

/**
 * Apply rule action to element.
 */
esp_err_t theme_element_apply_rule_action(lv_obj_t *obj, element_type_t type,
                                            const char *action, const char *value);

/**
 * Helper: round value per editor convention R(v) = floor(v + 0.5).
 */
static inline int16_t theme_round(float v) {
    return (int16_t)(v + 0.5f);
}

#ifdef __cplusplus
}
#endif
