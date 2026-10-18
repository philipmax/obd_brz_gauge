/* Host only: include the real private conversion; rename public symbols.
 * The original resample loop and axis builder below retain the old allocation
 * bound. Pixel fetching and axis cleanup are shared and unchanged.
 * Compare capacity and channel conversion against frozen baselines; shared
 * fetch/free helpers are not independently revalidated here.
 */
#include "resample_probe.h"
#define tpx_bpp rs_probe_bpp
#define tpx_validate rs_probe_validate
#define tpx_plan rs_probe_plan
#define tpx_render rs_probe_render
#define tpx_recolor rs_probe_recolor
#define tpx_img_free rs_probe_img_free
#define tpx_resample rs_actual
#ifndef RS_SOURCE
#define RS_SOURCE "../../../main/theme_engine/theme_pixconv.c"
#endif
#include RS_SOURCE

static bool rs_old_axis_build(const tpx_alloc_t *a, uint32_t sn, uint32_t dn, px_axis_t *ax)
{
    memset(ax, 0, sizeof(*ax));
    size_t cap = (size_t)sn + 2u * dn + 2u;
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

uint8_t rs_actual_u8(float v) { return px_u8(v); }
uint8_t rs_reference_u8(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)floorf(v + 0.5f);
}

tpx_err_t rs_reference(const tpx_src_t *src, uint16_t dw, uint16_t dh,
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
    if (!rs_old_axis_build(alloc, src->w, dw, &ax)) {
        px_free(alloc, dst);
        return TPX_ERR_NOMEM;
    }
    if (!rs_old_axis_build(alloc, src->h, dh, &ay)) {
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
            const uint8_t alpha = rs_reference_u8(pa);   // weights sum to 1
            if (alpha == 0 || pa <= 0.0f) {
                q[0] = q[1] = q[2] = q[3] = 0;
            } else {
                q[0] = rs_reference_u8(pr / pa);
                q[1] = rs_reference_u8(pg / pa);
                q[2] = rs_reference_u8(pb / pa);
                q[3] = alpha;
            }
        }
    }

    px_axis_free(alloc, &ax);
    px_axis_free(alloc, &ay);
    *out = dst;
    return TPX_OK;
}

/* Immediately preceding production loop: new rounding, old axis capacity. */
tpx_err_t rs_previous(const tpx_src_t *src, uint16_t dw, uint16_t dh,
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
    if (!rs_old_axis_build(alloc, src->w, dw, &ax)) {
        px_free(alloc, dst);
        return TPX_ERR_NOMEM;
    }
    if (!rs_old_axis_build(alloc, src->h, dh, &ay)) {
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

/* Private axis table comparison includes the full used tap sequence. */
static void *rs_trace_alloc(size_t n, void *ctx)
{
    *(size_t *)ctx += n;
    return malloc(n);
}
static void rs_trace_free(void *p, void *ctx) { (void)ctx; free(p); }
bool rs_axis_equal(uint16_t sn, uint16_t dn, size_t *old_bytes, size_t *new_bytes)
{
    *old_bytes = 0; *new_bytes = 0;
    tpx_alloc_t aa = {rs_trace_alloc, rs_trace_free, new_bytes};
    tpx_alloc_t ab = {rs_trace_alloc, rs_trace_free, old_bytes};
    px_axis_t actual, old;
    if (!px_axis_build(&aa, sn, dn, &actual)) return false;
    if (!rs_old_axis_build(&ab, sn, dn, &old)) {
        px_axis_free(NULL, &actual);
        return false;
    }
    size_t used_a = actual.start[dn - 1] + actual.count[dn - 1];
    size_t used_b = old.start[dn - 1] + old.count[dn - 1];
    bool ok = used_a == used_b &&
              !memcmp(actual.start, old.start, dn * sizeof(*old.start)) &&
              !memcmp(actual.count, old.count, dn * sizeof(*old.count)) &&
              !memcmp(actual.idx, old.idx, used_b * sizeof(*old.idx)) &&
              !memcmp(actual.w, old.w, used_b * sizeof(*old.w));
    px_axis_free(NULL, &actual); px_axis_free(NULL, &old);
    return ok;
}
