/* Original planner kept out of the firmware: per-pixel circle baseline. */
#include "plan_reference.h"
#include <string.h>

// Original per-pixel circle classification (before the row-span change).
static bool px_outside_round(int32_t sx, int32_t sy)
{
    int64_t dx = 2 * (int64_t)sx + 1 - TPX_SCREEN_DIM;
    int64_t dy = 2 * (int64_t)sy + 1 - TPX_SCREEN_DIM;
    return dx * dx + dy * dy > (int64_t)TPX_SCREEN_DIM * TPX_SCREEN_DIM;
}

static int32_t px_max(int32_t a, int32_t b) { return a > b ? a : b; }

tpx_err_t tpx_plan_reference(const tpx_src_t *src, const tpx_opts_t *opts, tpx_plan_t *plan)
{
    if (!plan) return TPX_ERR_ARG;
    memset(plan, 0, sizeof(*plan));
    if (!src || !opts || !src->data) return TPX_ERR_ARG;
    if (!tpx_validate(src->fmt, src->w, src->h, src->size)) return TPX_ERR_ARG;

    int64_t wx1 = (int64_t)opts->win_x + opts->win_w;
    int64_t wy1 = (int64_t)opts->win_y + opts->win_h;
    int32_t x0 = px_max(opts->win_x, 0);
    int32_t y0 = px_max(opts->win_y, 0);
    int32_t x1 = (int32_t)(wx1 < src->w ? wx1 : src->w);
    int32_t y1 = (int32_t)(wy1 < src->h ? wy1 : src->h);
    if (opts->win_w <= 0 || opts->win_h <= 0 || x1 <= x0 || y1 <= y0) {
        plan->cf = TPX_CF_NONE;
        return TPX_OK;
    }

    if (src->fmt == TPX_FMT_RGB565) {
        plan->cf = TPX_CF_TRUE_COLOR;
        plan->ox = x0;
        plan->oy = y0;
        plan->w = (uint16_t)(x1 - x0);
        plan->h = (uint16_t)(y1 - y0);
        plan->size = (size_t)plan->w * plan->h * 2u;
        plan->zero_copy = (x0 == 0 && x1 == (int32_t)src->w);
        return TPX_OK;
    }

    // rgba8888: bounding box of visible, non-transparent pixels
    const bool dc_on = opts->round_dontcare;
    int32_t bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = -1, by1 = -1;
    for (int32_t y = y0; y < y1; y++) {
        const uint8_t *p = src->data + ((size_t)y * src->w + (size_t)x0) * 4u;
        for (int32_t x = x0; x < x1; x++, p += 4) {
            if (p[3] == 0) continue;
            if (dc_on && px_outside_round(opts->scr_x + x, opts->scr_y + y)) continue;
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    }
    if (bx1 < 0) {
        plan->cf = TPX_CF_NONE;
        return TPX_OK;
    }

    int32_t cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;  // exclusive end
    if (opts->crop == TPX_CROP_ALL) {
        cx0 = bx0; cy0 = by0; cx1 = bx1 + 1; cy1 = by1 + 1;
    } else if (opts->crop == TPX_CROP_RB) {
        cx1 = bx1 + 1; cy1 = by1 + 1;
    }

    // Classify the cropped area again: opaque everywhere it is visible?
    bool opaque = true;
    for (int32_t y = cy0; y < cy1 && opaque; y++) {
        const uint8_t *p = src->data + ((size_t)y * src->w + (size_t)cx0) * 4u;
        for (int32_t x = cx0; x < cx1; x++, p += 4) {
            if (p[3] == 255) continue;
            if (dc_on && px_outside_round(opts->scr_x + x, opts->scr_y + y)) continue;
            opaque = false;
            break;
        }
    }

    plan->cf = opaque ? TPX_CF_TRUE_COLOR : TPX_CF_RGB565A8;
    plan->ox = cx0;
    plan->oy = cy0;
    plan->w = (uint16_t)(cx1 - cx0);
    plan->h = (uint16_t)(cy1 - cy0);
    plan->size = (size_t)plan->w * plan->h * (opaque ? 2u : 3u);
    plan->zero_copy = false;
    return TPX_OK;
}

