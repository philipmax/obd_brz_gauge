// ============================================================
//  theme_pixconv.c — theme.bin pixel conversion (see theme_pixconv.h)
// ============================================================

#include "theme_pixconv.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

static void *px_alloc(const tpx_alloc_t *a, size_t size)
{
    if (size == 0) size = 1;
    if (a && a->alloc_fn) return a->alloc_fn(size, a->ctx);
    return malloc(size);
}

static void px_free(const tpx_alloc_t *a, void *p)
{
    if (!p) return;
    if (a && a->alloc_fn) {
        if (a->free_fn) a->free_fn(p, a->ctx);
        return;
    }
    free(p);
}

size_t tpx_bpp(tpx_fmt_t fmt)
{
    return fmt == TPX_FMT_RGBA8888 ? 4u : 2u;
}

bool tpx_validate(tpx_fmt_t fmt, uint32_t w, uint32_t h, size_t size)
{
    if (fmt != TPX_FMT_RGB565 && fmt != TPX_FMT_RGBA8888) return false;
    if (w < 1 || h < 1 || w > TPX_MAX_DIM || h > TPX_MAX_DIM) return false;
    return (uint64_t)w * h * tpx_bpp(fmt) == (uint64_t)size;
}

// Classify the rectangular window before scanning alpha: 0 = outside,
// 1 = fully inside, 2 = intersects the rim. Keep screen-coordinate sums
// wide and reject out-of-panel coordinates before squaring distances.
// Once clipped to the 360 px panel, squared distances fit in int32_t.
static int px_round_window(const tpx_opts_t *opts, int32_t x0, int32_t y0,
                           int32_t x1, int32_t y1)
{
    const int64_t sx0 = (int64_t)opts->scr_x + x0, sx1 = (int64_t)opts->scr_x + x1 - 1;
    const int64_t sy0 = (int64_t)opts->scr_y + y0, sy1 = (int64_t)opts->scr_y + y1 - 1;
    const int64_t mid = TPX_SCREEN_DIM / 2 - 1;
    const int64_t sx = sx0 > mid ? sx0 : sx1 < mid ? sx1 : mid;
    const int64_t sy = sy0 > mid ? sy0 : sy1 < mid ? sy1 : mid;
    if (sx < 0 || sx >= TPX_SCREEN_DIM || sy < 0 || sy >= TPX_SCREEN_DIM) return 0;
    const int32_t dx = 2 * (int32_t)sx + 1 - TPX_SCREEN_DIM;
    const int32_t dy = 2 * (int32_t)sy + 1 - TPX_SCREEN_DIM;
    const int32_t radius2 = TPX_SCREEN_DIM * TPX_SCREEN_DIM;
    if (dx * dx + dy * dy > radius2) return 0;
    if (sx0 >= 0 && sx1 < TPX_SCREEN_DIM && sy0 >= 0 && sy1 < TPX_SCREEN_DIM) {
        const int32_t ax = 2 * (int32_t)sx0 + 1 - TPX_SCREEN_DIM;
        const int32_t bx = 2 * (int32_t)sx1 + 1 - TPX_SCREEN_DIM;
        const int32_t ay = 2 * (int32_t)sy0 + 1 - TPX_SCREEN_DIM;
        const int32_t by = 2 * (int32_t)sy1 + 1 - TPX_SCREEN_DIM;
        const int32_t fx2 = ax * ax > bx * bx ? ax * ax : bx * bx;
        const int32_t fy2 = ay * ay > by * by ? ay * ay : by * by;
        if (fx2 + fy2 <= radius2) return 1;
    }
    return 2;
}

// Intersect one source row with the round panel. Find the first visible
// pixel in the left half; symmetry gives the exclusive right edge. Integer
// pixel-center geometry matches the old per-pixel test, including the rim.
static bool px_round_span(const tpx_opts_t *opts, int32_t y, int32_t *x0, int32_t *x1)
{
    const int64_t sy = (int64_t)opts->scr_y + y;
    if (sy < 0 || sy >= TPX_SCREEN_DIM) return false;
    const int32_t dy = 2 * (int32_t)sy + 1 - TPX_SCREEN_DIM;
    int32_t lo = 0, hi = TPX_SCREEN_DIM / 2;
    while (lo < hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        const int32_t dx = 2 * mid + 1 - TPX_SCREEN_DIM;
        if (dx * dx + dy * dy > TPX_SCREEN_DIM * TPX_SCREEN_DIM) lo = mid + 1;
        else hi = mid;
    }
    int64_t start = (int64_t)lo - opts->scr_x;
    int64_t end = (int64_t)TPX_SCREEN_DIM - lo - opts->scr_x;
    if (start < *x0) start = *x0;
    if (end > *x1) end = *x1;
    if (start >= end) return false;
    *x0 = (int32_t)start;
    *x1 = (int32_t)end;
    return true;
}

static int32_t px_max(int32_t a, int32_t b) { return a > b ? a : b; }

tpx_err_t tpx_plan(const tpx_src_t *src, const tpx_opts_t *opts, tpx_plan_t *plan)
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
    const int round_window = opts->round_dontcare ? px_round_window(opts, x0, y0, x1, y1) : 1;
    if (round_window == 0) return TPX_OK;  // initialized empty plan
    const bool dc_on = round_window == 2;
    int32_t bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = -1, by1 = -1;
    for (int32_t y = y0; y < y1; y++) {
        int32_t rx0 = x0, rx1 = x1;
        if (dc_on && !px_round_span(opts, y, &rx0, &rx1)) continue;
        // Only the first and last alpha>0 pixels in a row can expand
        // its bounds. Avoid checking/updating bounds for every opaque pixel.
        const uint8_t *p = src->data + ((size_t)y * src->w + (size_t)rx0) * 4u;
        if (p[3] == 0) {
            // Skip transparent runs four alpha bytes at a time. Read only
            // complete groups inside the visible interval, then its tail.
            while (rx1 - rx0 >= 4 && (p[3] | p[7] | p[11] | p[15]) == 0) {
                rx0 += 4; p += 16;
            }
            while (rx0 < rx1 && p[3] == 0) { rx0++; p += 4; }
            if (rx0 == rx1) continue;
        }
        int32_t last = rx1 - 1;
        p = src->data + ((size_t)y * src->w + (size_t)last) * 4u;
        while (last > rx0 && p[3] == 0) { last--; p -= 4; }
        if (rx0 < bx0) bx0 = rx0;
        if (last > bx1) bx1 = last;
        if (y < by0) by0 = y;
        if (y > by1) by1 = y;
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
        int32_t rx0 = cx0, rx1 = cx1;
        if (dc_on && !px_round_span(opts, y, &rx0, &rx1)) continue;
        const uint8_t *p = src->data + ((size_t)y * src->w + (size_t)rx0) * 4u;
        for (int32_t x = rx0; x < rx1; x++, p += 4) {
            if (p[3] == 255) continue;
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

static inline void px_put565(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v >> 8);
    dst[1] = (uint8_t)(v & 0xFF);
}

// Color of a transparent pixel: average of its alpha>0 8-neighbors.
static uint16_t px_bleed(const tpx_src_t *src, int32_t x, int32_t y)
{
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for (int32_t dy = -1; dy <= 1; dy++) {
        int32_t yy = y + dy;
        if (yy < 0 || yy >= (int32_t)src->h) continue;
        for (int32_t dx = -1; dx <= 1; dx++) {
            int32_t xx = x + dx;
            if ((dx == 0 && dy == 0) || xx < 0 || xx >= (int32_t)src->w) continue;
            const uint8_t *q = src->data + ((size_t)yy * src->w + (size_t)xx) * 4u;
            if (q[3] == 0) continue;
            r += q[0]; g += q[1]; b += q[2]; n++;
        }
    }
    if (n == 0) {
        const uint8_t *p = src->data + ((size_t)y * src->w + (size_t)x) * 4u;
        return tpx_rgb565(p[0], p[1], p[2]);
    }
    return tpx_rgb565((uint8_t)((r + n / 2) / n), (uint8_t)((g + n / 2) / n),
                      (uint8_t)((b + n / 2) / n));
}

tpx_err_t tpx_render(const tpx_src_t *src, const tpx_plan_t *plan,
                     const tpx_alloc_t *alloc, tpx_img_t *out)
{
    if (!out) return TPX_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (!src || !plan || !src->data) return TPX_ERR_ARG;
    if (!tpx_validate(src->fmt, src->w, src->h, src->size)) return TPX_ERR_ARG;
    out->plan = *plan;
    if (plan->cf == TPX_CF_NONE) return TPX_OK;

    if (plan->ox < 0 || plan->oy < 0 || plan->w == 0 || plan->h == 0 ||
        (int64_t)plan->ox + plan->w > src->w || (int64_t)plan->oy + plan->h > src->h) {
        return TPX_ERR_ARG;
    }

    const size_t npx = (size_t)plan->w * plan->h;

    if (src->fmt == TPX_FMT_RGB565) {
        if (plan->cf != TPX_CF_TRUE_COLOR) return TPX_ERR_ARG;
        const uint8_t *first = src->data + ((size_t)plan->oy * src->w + (size_t)plan->ox) * 2u;
        if (plan->zero_copy) {
            if (plan->ox != 0 || plan->w != src->w) return TPX_ERR_ARG;
            out->data = first;
            return TPX_OK;
        }
        uint8_t *dst = px_alloc(alloc, npx * 2u);
        if (!dst) return TPX_ERR_NOMEM;
        for (uint32_t y = 0; y < plan->h; y++) {
            memcpy(dst + (size_t)y * plan->w * 2u, first + (size_t)y * src->w * 2u,
                   (size_t)plan->w * 2u);
        }
        out->owned = dst;
        out->data = dst;
        return TPX_OK;
    }

    if (plan->zero_copy) return TPX_ERR_ARG;
    const bool with_alpha = plan->cf == TPX_CF_RGB565A8;
    uint8_t *dst = px_alloc(alloc, npx * (with_alpha ? 3u : 2u));
    if (!dst) return TPX_ERR_NOMEM;
    uint8_t *aplane = with_alpha ? dst + npx * 2u : NULL;

    size_t i = 0;
    for (int32_t y = plan->oy; y < plan->oy + plan->h; y++) {
        const uint8_t *p = src->data + ((size_t)y * src->w + (size_t)plan->ox) * 4u;
        for (int32_t x = plan->ox; x < plan->ox + plan->w; x++, p += 4, i++) {
            uint16_t c;
            if (with_alpha && p[3] == 0) c = px_bleed(src, x, y);
            else c = tpx_rgb565(p[0], p[1], p[2]);
            px_put565(dst + i * 2u, c);
            if (aplane) aplane[i] = p[3];
        }
    }
    out->owned = dst;
    out->data = dst;
    return TPX_OK;
}

// LV_UDIV255(premult + c * (255 - opa) + round_ofs), per 5/6/5-bit channel.
static inline uint32_t px_mix(uint32_t premult, uint32_t c, uint32_t inv, uint32_t ofs)
{
    return ((premult + c * inv + ofs) * 0x8081u) >> 23;
}

void tpx_recolor(uint8_t *plane, size_t npx, uint32_t rgb, uint8_t opa, uint32_t round_ofs)
{
    if (!plane) return;
    const uint32_t r5 = ((rgb >> 16) & 0xFFu) >> 3;
    const uint32_t g6 = ((rgb >> 8) & 0xFFu) >> 2;
    const uint32_t b5 = (rgb & 0xFFu) >> 3;
    const uint32_t pr = r5 * opa, pg = g6 * opa, pb = b5 * opa;
    const uint32_t inv = 255u - opa;
    for (size_t i = 0; i < npx; i++) {
        uint8_t *p = plane + i * 2u;
        const uint32_t v = ((uint32_t)p[0] << 8) | p[1];
        const uint32_t r = px_mix(pr, (v >> 11) & 0x1Fu, inv, round_ofs) & 0x1Fu;
        const uint32_t g = px_mix(pg, (v >> 5) & 0x3Fu, inv, round_ofs) & 0x3Fu;
        const uint32_t b = px_mix(pb, v & 0x1Fu, inv, round_ofs) & 0x1Fu;
        px_put565(p, (uint16_t)((r << 11) | (g << 5) | b));
    }
}

void tpx_img_free(tpx_img_t *img, const tpx_alloc_t *alloc)
{
    if (!img) return;
    px_free(alloc, img->owned);
    memset(img, 0, sizeof(*img));
}

// ---------------- resampling ----------------

typedef struct {
    uint32_t *start;    // [dn] first tap
    uint16_t *count;    // [dn]
    uint16_t *idx;      // taps
    float *w;
} px_axis_t;

static void px_axis_free(const tpx_alloc_t *a, px_axis_t *ax)
{
    px_free(a, ax->start);
    px_free(a, ax->count);
    px_free(a, ax->idx);
    px_free(a, ax->w);
    memset(ax, 0, sizeof(*ax));
}

static bool px_axis_build(const tpx_alloc_t *a, uint32_t sn, uint32_t dn, px_axis_t *ax)
{
    memset(ax, 0, sizeof(*ax));
    // Identity needs one tap/output; bilinear stores exactly two. For area
    // sampling, the union of source/output boundaries gives at most sn+dn-1
    // intervals. Integer shrink ratios have exact double boundaries: sn taps.
    size_t cap = sn == dn ? dn : sn < dn ? 2u * dn :
                 sn % dn == 0 ? sn : (size_t)sn + dn - 1u;
    ax->start = px_alloc(a, dn * sizeof(uint32_t));
    ax->count = px_alloc(a, dn * sizeof(uint16_t));
    ax->idx = px_alloc(a, cap * sizeof(uint16_t));
    ax->w = px_alloc(a, cap * sizeof(float));
    if (!ax->start || !ax->count || !ax->idx || !ax->w) {
        px_axis_free(a, ax);
        return false;
    }
    size_t t = 0;
    for (uint32_t i = 0; i < dn; i++) {
        ax->start[i] = (uint32_t)t;
        if (sn == dn) {
            ax->idx[t] = (uint16_t)i;
            ax->w[t] = 1.0f;
            t++;
        } else if (dn < sn) {
            // area coverage of [i*s, (i+1)*s)
            double s = (double)sn / (double)dn;
            double lo = (double)i * s, hi = (double)(i + 1) * s;
            uint32_t j0 = (uint32_t)floor(lo);
            uint32_t j1 = (uint32_t)ceil(hi);
            if (j1 > sn) j1 = sn;
            for (uint32_t j = j0; j < j1 && t < cap; j++) {
                double a0 = (double)j > lo ? (double)j : lo;
                double a1 = (double)(j + 1) < hi ? (double)(j + 1) : hi;
                if (a1 <= a0) continue;
                ax->idx[t] = (uint16_t)j;
                ax->w[t] = (float)((a1 - a0) / s);
                t++;
            }
        } else {
            // bilinear at the output pixel center
            double sx = ((double)i + 0.5) * (double)sn / (double)dn - 0.5;
            double fl = floor(sx);
            double f = sx - fl;
            int32_t j0 = (int32_t)fl, j1 = j0 + 1;
            if (j0 < 0) j0 = 0;
            if (j1 < 0) j1 = 0;
            if (j0 > (int32_t)sn - 1) j0 = (int32_t)sn - 1;
            if (j1 > (int32_t)sn - 1) j1 = (int32_t)sn - 1;
            ax->idx[t] = (uint16_t)j0;
            ax->w[t] = (float)(1.0 - f);
            t++;
            ax->idx[t] = (uint16_t)j1;
            ax->w[t] = (float)f;
            t++;
        }
        ax->count[i] = (uint16_t)(t - ax->start[i]);
    }
    return true;
}

static inline void px_fetch(const tpx_src_t *src, uint32_t x, uint32_t y,
                            float *r, float *g, float *b, float *a)
{
    if (src->fmt == TPX_FMT_RGBA8888) {
        const uint8_t *p = src->data + ((size_t)y * src->w + x) * 4u;
        *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
    } else {
        const uint8_t *p = src->data + ((size_t)y * src->w + x) * 2u;
        uint16_t v = (uint16_t)((p[0] << 8) | p[1]);
        uint32_t r5 = (v >> 11) & 0x1Fu, g6 = (v >> 5) & 0x3Fu, b5 = v & 0x1Fu;
        *r = (float)((r5 << 3) | (r5 >> 2));
        *g = (float)((g6 << 2) | (g6 >> 4));
        *b = (float)((b5 << 3) | (b5 >> 2));
        *a = 255.0f;
    }
}

static uint8_t px_u8(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    /* The guards bound the float sum to [0.5, 255.5); truncation equals floor. */
    return (uint8_t)(v + 0.5f);
}

tpx_err_t tpx_resample(const tpx_src_t *src, uint16_t dw, uint16_t dh,
                       const tpx_alloc_t *alloc, uint8_t **out)
{
    if (!out) return TPX_ERR_ARG;
    *out = NULL;
    if (!src || !src->data) return TPX_ERR_ARG;
    if (!tpx_validate(src->fmt, src->w, src->h, src->size)) return TPX_ERR_ARG;
    if (dw < 1 || dh < 1 || dw > TPX_MAX_DIM || dh > TPX_MAX_DIM) return TPX_ERR_ARG;

    uint8_t *dst = px_alloc(alloc, (size_t)dw * dh * 4u);
    if (!dst) return TPX_ERR_NOMEM;

    if (dw == src->w && dh == src->h) {
        if (src->fmt == TPX_FMT_RGBA8888) {
            memcpy(dst, src->data, (size_t)dw * dh * 4u);
        } else {
            for (uint32_t y = 0; y < dh; y++) {
                for (uint32_t x = 0; x < dw; x++) {
                    float r, g, b, a;
                    px_fetch(src, x, y, &r, &g, &b, &a);
                    uint8_t *q = dst + ((size_t)y * dw + x) * 4u;
                    q[0] = (uint8_t)r; q[1] = (uint8_t)g; q[2] = (uint8_t)b; q[3] = 255;
                }
            }
        }
        *out = dst;
        return TPX_OK;
    }

    px_axis_t ax, ay;
    if (!px_axis_build(alloc, src->w, dw, &ax)) {
        px_free(alloc, dst);
        return TPX_ERR_NOMEM;
    }
    if (!px_axis_build(alloc, src->h, dh, &ay)) {
        px_axis_free(alloc, &ax);
        px_free(alloc, dst);
        return TPX_ERR_NOMEM;
    }

    for (uint32_t oy = 0; oy < dh; oy++) {
        const uint32_t ys = ay.start[oy], yn = ay.count[oy];
        for (uint32_t ox = 0; ox < dw; ox++) {
            const uint32_t xs = ax.start[ox], xn = ax.count[ox];
            float pr = 0, pg = 0, pb = 0, pa = 0;
            for (uint32_t j = 0; j < yn; j++) {
                const float wy = ay.w[ys + j];
                if (wy == 0.0f) continue;
                const uint32_t sy = ay.idx[ys + j];
                for (uint32_t k = 0; k < xn; k++) {
                    const float wx = ax.w[xs + k];
                    if (wx == 0.0f) continue;
                    float r, g, b, a;
                    px_fetch(src, ax.idx[xs + k], sy, &r, &g, &b, &a);
                    const float wa = wx * wy * a;
                    pr += wa * r;
                    pg += wa * g;
                    pb += wa * b;
                    pa += wa;
                }
            }
            uint8_t *q = dst + ((size_t)oy * dw + ox) * 4u;
            const uint8_t alpha = px_u8(pa);   // weights sum to 1
            if (alpha == 0 || pa <= 0.0f) {
                q[0] = q[1] = q[2] = q[3] = 0;
            } else {
                q[0] = px_u8(pr / pa);
                q[1] = px_u8(pg / pa);
                q[2] = px_u8(pb / pa);
                q[3] = alpha;
            }
        }
    }

    px_axis_free(alloc, &ax);
    px_axis_free(alloc, &ay);
    *out = dst;
    return TPX_OK;
}
