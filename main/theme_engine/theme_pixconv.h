#pragma once
// ============================================================
//  theme_pixconv.h — theme.bin pixel conversion (pure C99)
//
//  No ESP-IDF / LVGL dependency, host-testable (test/host/theme).
//  REFACTOR_PLAN.md 5.7:
//    - editor rgba8888 (straight RGBA, 4 B/px) → LVGL 8.4
//      LV_IMG_CF_RGB565A8 (big-endian RGB565 plane, then A8 plane,
//      3 B/px) or LV_IMG_CF_TRUE_COLOR (2 B/px) when every visible
//      pixel is opaque;
//    - editor rgb565 (big-endian = LV_COLOR_16_SWAP memory order) is
//      used zero-copy when the visible window spans full rows;
//    - only the visible window is converted, the round screen's
//      corners (r > 180 px from (180,180)) are don't-care, the result
//      is cropped to the alpha>0 bounding box and classified again;
//    - truncation quantization (editor formula), no premultiply;
//    - one-pass alpha bleed so rotated/filtered edges stay clean.
//  Pointer sprites are resampled to the element box first
//  (tpx_resample, premultiplied weighted sampling).
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TPX_MAX_DIM     2047      // theme.bin image width/height limit
#define TPX_SCREEN_DIM  360       // round panel diameter

typedef enum {
    TPX_FMT_RGB565 = 0,           // big-endian, 2 B/px, opaque
    TPX_FMT_RGBA8888 = 1,         // straight RGBA, 4 B/px
} tpx_fmt_t;

typedef enum {
    TPX_CF_NONE = 0,              // nothing visible (fully transparent)
    TPX_CF_TRUE_COLOR,            // RGB565 big-endian, 2 B/px
    TPX_CF_RGB565A8,              // RGB565 BE plane + A8 plane, 3 B/px
} tpx_cf_t;

typedef enum {
    TPX_CROP_NONE = 0,            // output = window
    TPX_CROP_RB,                  // shrink right/bottom only (keeps origin, e.g. rotated images)
    TPX_CROP_ALL,                 // shrink to the alpha>0 bounding box
} tpx_crop_t;

typedef enum {
    TPX_OK = 0,
    TPX_ERR_ARG,
    TPX_ERR_NOMEM,
} tpx_err_t;

// NULL alloc_fn → malloc/free.
typedef struct {
    void *(*alloc_fn)(size_t size, void *ctx);
    void (*free_fn)(void *ptr, void *ctx);
    void *ctx;
} tpx_alloc_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    tpx_fmt_t fmt;
    uint16_t w, h;
} tpx_src_t;

typedef struct {
    // Window in source pixels; clipped to the image.
    int32_t win_x, win_y, win_w, win_h;
    // Treat pixels outside the round screen as don't-care. scr_x/scr_y is
    // the screen position of source pixel (0,0). Only valid for unrotated
    // images.
    bool round_dontcare;
    int32_t scr_x, scr_y;
    uint8_t crop;                 // tpx_crop_t
} tpx_opts_t;

typedef struct {
    tpx_cf_t cf;
    uint16_t w, h;                // output size (0 with TPX_CF_NONE)
    int32_t ox, oy;               // source coords of output pixel (0,0)
    size_t size;                  // bytes the output occupies
    bool zero_copy;               // output points into the source (rgb565, full rows)
} tpx_plan_t;

typedef struct {
    tpx_plan_t plan;
    const uint8_t *data;          // NULL with TPX_CF_NONE
    uint8_t *owned;               // allocation to free (NULL when zero-copy)
} tpx_img_t;

size_t tpx_bpp(tpx_fmt_t fmt);

// size == w*h*bpp, 1 <= w,h <= TPX_MAX_DIM.
bool tpx_validate(tpx_fmt_t fmt, uint32_t w, uint32_t h, size_t size);

// Classify the window and compute the output geometry without allocating.
// Round don't-care RGBA scans use integer row spans; fully outside/inside
// windows are classified first; alpha bounds use the first/last visible
// nontransparent pixel per row. Pixel-center rim, crop and alpha semantics
// are identical to checking the circle for every pixel.
tpx_err_t tpx_plan(const tpx_src_t *src, const tpx_opts_t *opts, tpx_plan_t *plan);

// Produce the output described by `plan` (from tpx_plan with the same src).
tpx_err_t tpx_render(const tpx_src_t *src, const tpx_plan_t *plan,
                     const tpx_alloc_t *alloc, tpx_img_t *out);

void tpx_img_free(tpx_img_t *img, const tpx_alloc_t *alloc);

// Resample src to dw x dh straight RGBA8888 (*out, dw*dh*4 bytes, from alloc).
// Area coverage when shrinking an axis, bilinear when enlarging, alpha
// premultiplied while filtering. Same size → plain copy/expansion.
tpx_err_t tpx_resample(const tpx_src_t *src, uint16_t dw, uint16_t dh,
                       const tpx_alloc_t *alloc, uint8_t **out);

// Editor quantization: (r&0xF8)<<8 | (g&0xFC)<<3 | b>>3.
static inline uint16_t tpx_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

// Bake an image recolor into npx big-endian RGB565 pixels in place (the
// color plane of TRUE_COLOR / RGB565A8 output; alpha untouched). Same
// integer math as LVGL 8.4 drawing with img_recolor = rgb (0xRRGGBB,
// truncated like lv_color_hex) and img_recolor_opa = opa at 16-bit depth:
// lv_color_premult + lv_color_mix_premult, round_ofs =
// LV_COLOR_MIX_ROUND_OFS. The baked image drawn without recolor is
// pixel-identical to the original drawn with it.
void tpx_recolor(uint8_t *plane, size_t npx, uint32_t rgb, uint8_t opa, uint32_t round_ofs);

#ifdef __cplusplus
}
#endif
