/* Row-clipped planner must match the original per-pixel circle classifier. */
#include "plan_reference.h"
#include "th_util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = UINT32_C(0x70a9413b);
static uint32_t next(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
static void compare(const tpx_src_t *src, const tpx_opts_t *opts)
{
    tpx_plan_t a, b;
    TH_CHECK(tpx_plan_reference(src, opts, &a) == tpx_plan(src, opts, &b));
    TH_CHECK(a.cf == b.cf);
    TH_CHECK(a.w == b.w && a.h == b.h);
    TH_CHECK(a.ox == b.ox && a.oy == b.oy);
    TH_CHECK(a.size == b.size && a.zero_copy == b.zero_copy);
    if (th_fail) {
        printf("seed=%08x src=%ux%u window=%d,%d,%d,%d screen=%d,%d crop=%u round=%d\n",
            rng, src->w, src->h, opts->win_x, opts->win_y, opts->win_w, opts->win_h,
            opts->scr_x, opts->scr_y, opts->crop, opts->round_dontcare);
        exit(1);
    }
}
static void test_rim(void)
{
    uint8_t *pixels = calloc(360u * 360u, 4u);
    TH_CHECK(pixels != NULL);
    if (!pixels) return;
    tpx_src_t src = {pixels, 360u * 360u * 4u, TPX_FMT_RGBA8888, 360, 360};
    tpx_opts_t opts = {0, 0, 360, 360, true, 0, 0, TPX_CROP_ALL};
    /* Every panel pixel as its own window: includes every tangent boundary. */
    for (int32_t y = 0; y < 360; y++) for (int32_t x = 0; x < 360; x++) {
        size_t i = ((size_t)y * 360u + (size_t)x) * 4u + 3u;
        pixels[i] = 255;
        opts.win_x = x; opts.win_y = y; opts.win_w = opts.win_h = 1;
        compare(&src, &opts);
        pixels[i] = 0;
    }
    /* Alpha on both sides of the rim, for every crop mode. */
    for (unsigned pattern = 0; pattern < 4; pattern++) {
        for (unsigned y = 0; y < 360; y++) for (unsigned x = 0; x < 360; x++) {
            int32_t dx = 2 * (int32_t)x + 1 - 360, dy = 2 * (int32_t)y + 1 - 360;
            bool outside = dx * dx + dy * dy > 360 * 360;
            pixels[((size_t)y * 360u + x) * 4u + 3u] = pattern == 0 ? 255 :
                pattern == 1 ? (outside ? 0 : 255) : pattern == 2 ? (outside ? 255 : 0) :
                outside ? 128 : 255;
        }
        for (unsigned crop = 0; crop < 3; crop++) {
            opts = (tpx_opts_t){0, 0, 360, 360, true, 0, 0, (uint8_t)crop};
            compare(&src, &opts);
        }
    }
    free(pixels);
}
static void test_replay(void)
{
    uint8_t pixels[96u * 96u * 4u];
    for (unsigned step = 0; step < 20000; step++) {
        unsigned w = next() % 96u + 1u, h = next() % 96u + 1u;
        unsigned pattern = step % 5u;
        for (unsigned i = 0; i < w * h; i++) {
            uint32_t r = next();
            pixels[i * 4u] = (uint8_t)r;
            pixels[i * 4u + 1u] = (uint8_t)(r >> 8);
            pixels[i * 4u + 2u] = (uint8_t)(r >> 16);
            pixels[i * 4u + 3u] = pattern == 0 ? 0 : pattern == 1 ? 255 :
                pattern == 2 ? 128 : pattern == 3 ? (r & 1u ? 255 : 0) : (uint8_t)(r >> 24);
        }
        tpx_src_t src = {pixels, (size_t)w * h * 4u, TPX_FMT_RGBA8888, (uint16_t)w, (uint16_t)h};
        tpx_opts_t opts = {(int32_t)(next() % 130u) - 30, (int32_t)(next() % 130u) - 30,
            (int32_t)(next() % 160u) - 5, (int32_t)(next() % 160u) - 5,
            step % 7u != 0, (int32_t)(next() % 600u) - 120,
            (int32_t)(next() % 600u) - 120, (uint8_t)(step % 3u)};
        if (step % 2u == 0) { opts.win_x = opts.win_y = 0; opts.win_w = (int32_t)w; opts.win_h = (int32_t)h; }
        compare(&src, &opts);
        if (step % 11u == 0) { src.fmt = TPX_FMT_RGB565; src.size /= 2u; compare(&src, &opts); }
    }
    /* Maximum dimension; only the panel intersection should affect the plan. */
    uint8_t *large = malloc(2047u * 2047u * 4u);
    TH_CHECK(large != NULL);
    if (!large) return;
    memset(large, 255, 2047u * 2047u * 4u);
    tpx_src_t src = {large, 2047u * 2047u * 4u, TPX_FMT_RGBA8888, 2047, 2047};
    for (unsigned crop = 0; crop < 3; crop++) {
        tpx_opts_t opts = {0, 0, 2047, 2047, true, -843, -843, (uint8_t)crop};
        compare(&src, &opts);
    }
    free(large);
}
static void test_screen_extremes(void)
{
    uint8_t pixel[] = {0, 0, 0, 255};
    tpx_src_t src = {pixel, sizeof(pixel), TPX_FMT_RGBA8888, 1, 1};
    static const int32_t positions[] = {INT32_MIN, INT32_MIN + 1, INT32_MAX - 1, INT32_MAX};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        tpx_plan_t p;
        tpx_opts_t opts = {0, 0, 1, 1, true, positions[i], 180, TPX_CROP_ALL};
        TH_CHECK(tpx_plan(&src, &opts, &p) == TPX_OK && p.cf == TPX_CF_NONE);
        opts.scr_x = 180; opts.scr_y = positions[i];
        TH_CHECK(tpx_plan(&src, &opts, &p) == TPX_OK && p.cf == TPX_CF_NONE);
    }
}
int main(void)
{
    test_rim(); test_replay(); test_screen_extremes();
    return th_report("theme_plan");
}
