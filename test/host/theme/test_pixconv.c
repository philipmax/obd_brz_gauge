// theme_pixconv: classification, crop policies, round-screen don't-care,
// RGB565A8 planar layout, zero-copy rgb565, alpha bleed and resampling.
#include <stdlib.h>
#include <string.h>

#include "theme_pixconv.h"
#include "th_util.h"

static int g_live;   // outstanding allocations of the counting allocator

static void *cnt_alloc(size_t n, void *ctx)
{
    (void)ctx;
    g_live++;
    return malloc(n);
}

static void cnt_free(void *p, void *ctx)
{
    (void)ctx;
    if (p) g_live--;
    free(p);
}

static const tpx_alloc_t A = {cnt_alloc, cnt_free, NULL};

static void set_px(uint8_t *img, int w, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t *p = img + ((size_t)y * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static tpx_opts_t win_all(int w, int h, uint8_t crop)
{
    tpx_opts_t o;
    memset(&o, 0, sizeof(o));
    o.win_w = w;
    o.win_h = h;
    o.crop = crop;
    return o;
}

static void test_validate(void)
{
    TH_CHECK(tpx_validate(TPX_FMT_RGB565, 3, 2, 12));
    TH_CHECK(!tpx_validate(TPX_FMT_RGB565, 3, 2, 13));
    TH_CHECK(tpx_validate(TPX_FMT_RGBA8888, 3, 2, 24));
    TH_CHECK(!tpx_validate(TPX_FMT_RGBA8888, 0, 2, 0));
    TH_CHECK(!tpx_validate(TPX_FMT_RGBA8888, 2048, 1, 2048 * 4));
    TH_CHECK(tpx_validate(TPX_FMT_RGB565, 2047, 1, 2047 * 2));
    TH_CHECK(!tpx_validate((tpx_fmt_t)7, 1, 1, 2));
    TH_CHECK(tpx_rgb565(255, 128, 64) == 0xFC08);
}

static void test_rgb565(void)
{
    uint8_t img[4 * 3 * 2];
    for (size_t i = 0; i < sizeof(img); i++) img[i] = (uint8_t)i;
    tpx_src_t s = {img, sizeof(img), TPX_FMT_RGB565, 4, 3};

    tpx_opts_t o = win_all(4, 3, TPX_CROP_ALL);
    o.win_y = 1;
    tpx_plan_t p;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK);
    TH_CHECK(p.cf == TPX_CF_TRUE_COLOR && p.zero_copy && p.w == 4 && p.h == 2 && p.oy == 1);
    tpx_img_t out;
    TH_CHECK(tpx_render(&s, &p, &A, &out) == TPX_OK);
    TH_CHECK(out.data == img + 8 && out.owned == NULL);
    tpx_img_free(&out, &A);

    // window narrower than the image → row copy
    o = win_all(2, 3, TPX_CROP_NONE);
    o.win_x = 1;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK);
    TH_CHECK(p.cf == TPX_CF_TRUE_COLOR && !p.zero_copy && p.w == 2 && p.h == 3 && p.ox == 1);
    TH_CHECK(tpx_render(&s, &p, &A, &out) == TPX_OK);
    TH_CHECK(out.owned && out.data[0] == 2 && out.data[3] == 5 && out.data[4] == 10);
    tpx_img_free(&out, &A);

    // window outside the image
    o = win_all(2, 2, TPX_CROP_NONE);
    o.win_x = 10;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK && p.cf == TPX_CF_NONE);
    TH_CHECK(g_live == 0);
}

static void test_rgba_classify(void)
{
    enum { W = 6, H = 5 };
    uint8_t img[W * H * 4];
    memset(img, 0, sizeof(img));
    tpx_src_t s = {img, sizeof(img), TPX_FMT_RGBA8888, W, H};
    tpx_plan_t p;
    tpx_img_t out;

    // fully transparent
    tpx_opts_t o = win_all(W, H, TPX_CROP_ALL);
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK && p.cf == TPX_CF_NONE);
    TH_CHECK(tpx_render(&s, &p, &A, &out) == TPX_OK && out.data == NULL);

    // opaque 2x2 block at (2,1)
    set_px(img, W, 2, 1, 255, 0, 0, 255);
    set_px(img, W, 3, 1, 255, 0, 0, 255);
    set_px(img, W, 2, 2, 255, 0, 0, 255);
    set_px(img, W, 3, 2, 0, 0, 255, 255);

    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK);
    TH_CHECK(p.cf == TPX_CF_TRUE_COLOR && p.ox == 2 && p.oy == 1 && p.w == 2 && p.h == 2 &&
             p.size == 8);
    TH_CHECK(tpx_render(&s, &p, &A, &out) == TPX_OK);
    TH_CHECK(be16(out.data) == 0xF800 && be16(out.data + 6) == 0x001F);
    tpx_img_free(&out, &A);

    // right/bottom crop keeps the origin and the transparent margin → alpha plane
    o.crop = TPX_CROP_RB;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK);
    TH_CHECK(p.cf == TPX_CF_RGB565A8 && p.ox == 0 && p.oy == 0 && p.w == 4 && p.h == 3 &&
             p.size == 4 * 3 * 3);
    TH_CHECK(tpx_render(&s, &p, &A, &out) == TPX_OK);
    const uint8_t *a = out.data + 4 * 3 * 2;
    TH_CHECK(a[0] == 0 && a[1 * 4 + 2] == 255 && a[2 * 4 + 3] == 255);
    // bleed: (1,1) is transparent, neighbors (2,1) and (2,2) are red → red
    TH_CHECK(be16(out.data + (1 * 4 + 1) * 2) == 0xF800);
    // (1,2): neighbors (2,1),(2,2) red → red; (4,?) outside the crop is not read for (3,*)
    // (0,0): no opaque neighbor → its own (black) color
    TH_CHECK(be16(out.data) == 0x0000);
    tpx_img_free(&out, &A);

    // no crop
    o.crop = TPX_CROP_NONE;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK && p.w == W && p.h == H && p.cf == TPX_CF_RGB565A8);

    // semi-transparent pixel inside the box → RGB565A8 even with full crop
    set_px(img, W, 3, 2, 0, 255, 0, 128);
    o.crop = TPX_CROP_ALL;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK && p.cf == TPX_CF_RGB565A8 && p.w == 2);
    TH_CHECK(tpx_render(&s, &p, &A, &out) == TPX_OK);
    TH_CHECK(out.data[4 * 2 + 3] == 128 && be16(out.data + 3 * 2) == 0x07E0);
    tpx_img_free(&out, &A);

    // window clips the bounding box search
    o = win_all(1, 5, TPX_CROP_ALL);
    o.win_x = 3;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK && p.ox == 3 && p.oy == 1 && p.w == 1 && p.h == 2);
    TH_CHECK(g_live == 0);
}

static void test_round_dontcare(void)
{
    // 3x1 strip at screen (-2,180): x=0,1 lie outside the circle, x=2 inside
    uint8_t img[3 * 4];
    memset(img, 0, sizeof(img));
    set_px(img, 3, 0, 0, 10, 10, 10, 0);
    set_px(img, 3, 1, 0, 10, 10, 10, 128);
    set_px(img, 3, 2, 0, 255, 255, 255, 255);
    tpx_src_t s = {img, sizeof(img), TPX_FMT_RGBA8888, 3, 1};
    tpx_opts_t o = win_all(3, 1, TPX_CROP_ALL);
    o.scr_x = -2;
    o.scr_y = 180;
    tpx_plan_t p;

    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK);
    TH_CHECK(p.cf == TPX_CF_RGB565A8 && p.ox == 1 && p.w == 2);

    o.round_dontcare = true;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK);
    TH_CHECK(p.cf == TPX_CF_TRUE_COLOR && p.ox == 2 && p.w == 1);

    // no crop: still opaque where it is visible
    o.crop = TPX_CROP_NONE;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK);
    TH_CHECK(p.cf == TPX_CF_TRUE_COLOR && p.ox == 0 && p.w == 3);

    // entirely in the corner
    o.scr_x = 0;
    o.scr_y = 0;
    TH_CHECK(tpx_plan(&s, &o, &p) == TPX_OK && p.cf == TPX_CF_NONE);
}

static void test_resample(void)
{
    // 2x2 → 1x1: three red opaque, one transparent blue (ignored by premultiply)
    uint8_t img[2 * 2 * 4];
    set_px(img, 2, 0, 0, 255, 0, 0, 255);
    set_px(img, 2, 1, 0, 0, 0, 255, 0);
    set_px(img, 2, 0, 1, 255, 0, 0, 255);
    set_px(img, 2, 1, 1, 255, 0, 0, 255);
    tpx_src_t s = {img, sizeof(img), TPX_FMT_RGBA8888, 2, 2};
    uint8_t *out = NULL;
    TH_CHECK(tpx_resample(&s, 1, 1, &A, &out) == TPX_OK);
    TH_CHECK(out && out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 191);
    cnt_free(out, NULL);

    // 2x2 → 3x1 horizontal enlarge, vertical shrink
    TH_CHECK(tpx_resample(&s, 3, 1, &A, &out) == TPX_OK);
    TH_CHECK(out && out[3] == 255 && out[0] == 255);    // left column fully opaque red
    TH_CHECK(out[11] == 128 && out[8] == 255);          // right column: half covered
    cnt_free(out, NULL);

    // identity copy
    TH_CHECK(tpx_resample(&s, 2, 2, &A, &out) == TPX_OK);
    TH_CHECK(out && memcmp(out, img, sizeof(img)) == 0);
    cnt_free(out, NULL);

    // rgb565 source expands to opaque RGBA
    uint8_t c565[2] = {0xF8, 0x00};
    tpx_src_t s2 = {c565, 2, TPX_FMT_RGB565, 1, 1};
    TH_CHECK(tpx_resample(&s2, 4, 3, &A, &out) == TPX_OK);
    TH_CHECK(out && out[0] == 255 && out[1] == 0 && out[3] == 255 && out[11 * 4] == 255);
    cnt_free(out, NULL);

    // uniform image keeps its color under any scale
    enum { W = 7, H = 5 };
    uint8_t u[W * H * 4];
    for (int i = 0; i < W * H; i++) {
        u[i * 4] = 12; u[i * 4 + 1] = 200; u[i * 4 + 2] = 99; u[i * 4 + 3] = 77;
    }
    tpx_src_t s3 = {u, sizeof(u), TPX_FMT_RGBA8888, W, H};
    const uint16_t sizes[][2] = {{3, 2}, {13, 11}, {7, 1}, {1, 9}};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        TH_CHECK(tpx_resample(&s3, sizes[k][0], sizes[k][1], &A, &out) == TPX_OK);
        bool ok = out != NULL;
        for (int i = 0; ok && i < sizes[k][0] * sizes[k][1]; i++) {
            ok = out[i * 4] == 12 && out[i * 4 + 1] == 200 && out[i * 4 + 2] == 99 &&
                 out[i * 4 + 3] == 77;
        }
        TH_CHECK(ok);
        cnt_free(out, NULL);
    }
    TH_CHECK(tpx_resample(&s3, 0, 1, &A, &out) == TPX_ERR_ARG && out == NULL);
    TH_CHECK(g_live == 0);
}

// LVGL 8.4 recolor (lv_color_premult + lv_color_mix_premult, 16-bit,
// LV_COLOR_MIX_ROUND_OFS 128) baked into the big-endian color plane.
static void test_recolor(void)
{
    static uint8_t plane[65536 * 2];
    for (uint32_t v = 0; v < 65536; v++) {
        plane[v * 2] = (uint8_t)(v >> 8);
        plane[v * 2 + 1] = (uint8_t)v;
    }
    // opa 0 is the identity for every 16-bit value
    tpx_recolor(plane, 65536, 0xFF3158, 0, 128);
    bool ok = true;
    for (uint32_t v = 0; v < 65536 && ok; v++) {
        ok = plane[v * 2] == (uint8_t)(v >> 8) && plane[v * 2 + 1] == (uint8_t)v;
    }
    TH_CHECK(ok);
    // opa 255 replaces every pixel with the truncated recolor
    tpx_recolor(plane, 65536, 0xFF3158, 255, 128);
    const uint16_t want = (uint16_t)((31u << 11) | (12u << 5) | 11u);
    ok = true;
    for (uint32_t v = 0; v < 65536 && ok; v++) {
        ok = plane[v * 2] == (uint8_t)(want >> 8) && plane[v * 2 + 1] == (uint8_t)want;
    }
    TH_CHECK(ok);
    // rule tint (opa 191) over black: r=(31*191+128)*0x8081>>23 = 23, g = 9, b = 8
    uint8_t px[4] = {0x00, 0x00, 0xFF, 0xFF};
    tpx_recolor(px, 2, 0xFF3158, 191, 128);
    TH_CHECK(px[0] == 0xB9 && px[1] == 0x28);
    // over white: r=(5921+31*64+128)>>.. = 31, g=(2292+63*64+128) = 25, b=(2101+31*64+128) = 16
    TH_CHECK(px[2] == (uint8_t)(((31u << 11) | (25u << 5) | 16u) >> 8) &&
             px[3] == (uint8_t)((31u << 11) | (25u << 5) | 16u));
    tpx_recolor(NULL, 4, 0, 191, 128);   // no-op
}

int main(void)
{
    test_validate();
    test_rgb565();
    test_rgba_classify();
    test_round_dontcare();
    test_resample();
    test_recolor();
    return th_report("pixconv");
}
