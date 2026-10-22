#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Theme elements are siblings of their own video wrapper. Compare the direct
 * page children so nested video objects retain the page's real z-order. */
static inline uint32_t video_elem_layer_order(lv_obj_t *obj)
{
    if (!obj) return 0;
    lv_obj_t *screen = lv_obj_get_screen(obj);
    lv_obj_t *layer = obj;
    while (lv_obj_get_parent(layer) && lv_obj_get_parent(layer) != screen)
        layer = lv_obj_get_parent(layer);
    return lv_obj_get_parent(layer) == screen ? lv_obj_get_index(layer) : 0;
}

typedef struct {
    uint32_t order;
    bool found;
} video_elem_layer_choice_t;

static inline bool video_elem_layer_consider(video_elem_layer_choice_t *choice,
                                              bool eligible, uint32_t order)
{
    if (!choice || !eligible || (choice->found && order < choice->order)) return false;
    choice->order = order;
    choice->found = true;
    return true;
}
