/* Compare real channel conversion and full resampling with the old floorf path. */
#include "resample_probe.h"
#include "th_util.h"
#include <float.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 0x914c5a37u;
static uint64_t scalar_count, image_count, image_bytes, axis_count, axis_old_bytes, axis_new_bytes;
static uint32_t next(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
static bool channel(uint32_t bits)
{
    float v;
    memcpy(&v, &bits, sizeof(v));
    scalar_count++;
    uint8_t a = rs_actual_u8(v), b = rs_reference_u8(v);
    if (a == b) return true;
    fprintf(stderr, "channel mismatch: bits=%08" PRIx32 " actual=%u reference=%u\n", bits, a, b);
    return false;
}
static void scalars(bool exhaustive)
{
    TH_CHECK(sizeof(float) == sizeof(uint32_t) && FLT_RADIX == 2 && FLT_MANT_DIG == 24);
    for (unsigned i = 0; i < 256; i++) {
        float v = (float)i + 0.5f;
        uint32_t bits; memcpy(&bits, &v, sizeof(bits));
        for (int delta = -64; delta <= 64; delta++) {
            if (!channel(bits + (uint32_t)delta)) { TH_CHECK(false); return; }
        }
    }
    const uint32_t special[] = {0, 1, 0x007fffff, 0x00800000, 0x3effffff, 0x3f000000,
        0x437effff, 0x437f0000, 0x437f0001, 0x7f7fffff, 0x7f800000, 0x7f800001,
        0x7fc00000, 0x7fffffff, 0x80000000, 0x80000001, 0xff800000, 0xffc00000};
    for (size_t i = 0; i < sizeof(special)/sizeof(special[0]); i++) {
        if (!channel(special[i])) { TH_CHECK(false); return; }
    }
    for (unsigned i = 0; i < 262144; i++) {
        if (!channel(next())) { TH_CHECK(false); return; }
    }
    if (exhaustive) {
        for (uint64_t b = 0; b <= UINT32_MAX; b++) {
            if (!channel((uint32_t)b)) { TH_CHECK(false); return; }
        }
    }
    TH_CHECK(true);
}

typedef struct { size_t calls, fail, live, bytes, peak; } mem_t;
static void *count_alloc(size_t size, void *ctx)
{
    mem_t *m = ctx;
    m->calls++;
    if (m->fail == m->calls) return NULL;
    size_t *p = malloc(sizeof(size_t) + size);
    if (p) {
        *p = size; m->live++; m->bytes += size;
        if (m->bytes > m->peak) m->peak = m->bytes;
        return p + 1;
    }
    return NULL;
}
static void count_free(void *ptr, void *ctx)
{
    if (ptr) {
        mem_t *m = ctx; size_t *p = (size_t *)ptr - 1;
        m->live--; m->bytes -= *p; free(p);
    }
}
static bool pair(const tpx_src_t *src, uint16_t dw, uint16_t dh, size_t fail)
{
    mem_t ma = {.fail = fail}, mb = {.fail = fail}, mc = {.fail = fail};
    tpx_alloc_t aa = {count_alloc, count_free, &ma}, ab = {count_alloc, count_free, &mb};
    tpx_alloc_t ac = {count_alloc, count_free, &mc};
    uint8_t *a = (void *)(uintptr_t)1, *b = (void *)(uintptr_t)1, *c = (void *)(uintptr_t)1;
    tpx_err_t ea = rs_actual(src, dw, dh, &aa, &a);
    tpx_err_t eb = rs_reference(src, dw, dh, &ab, &b);
    tpx_err_t ec = tpx_resample(src, dw, dh, &ac, &c);
    bool ok = ea == eb && ea == ec && ma.calls == mb.calls && ma.calls == mc.calls &&
              ma.live == mb.live && ma.live == mc.live &&
              ma.peak <= mb.peak && ma.peak == mc.peak;
    if (ea == TPX_OK && eb == TPX_OK && ec == TPX_OK) {
        size_t bytes = (size_t)dw * dh * 4;
        ok = ok && a && b && c && !memcmp(a, b, bytes) && !memcmp(a, c, bytes);
        image_count++; image_bytes += bytes;
    } else {
        ok = ok && a == NULL && b == NULL && c == NULL;
    }
    count_free(a, &ma); count_free(b, &mb); count_free(c, &mc);
    ok = ok && ma.live == 0 && mb.live == 0 && mc.live == 0 &&
               ma.bytes == 0 && mb.bytes == 0 && mc.bytes == 0;
    if (!ok) fprintf(stderr, "image mismatch: %ux%u -> %ux%u fmt=%u fail=%zu\n",
                     src ? src->w : 0, src ? src->h : 0, dw, dh,
                     src ? (unsigned)src->fmt : 0, fail);
    return ok;
}
static void image(uint16_t sw, uint16_t sh, uint16_t dw, uint16_t dh, tpx_fmt_t fmt,
                  unsigned pattern)
{
    size_t bytes = (size_t)sw * sh * (fmt == TPX_FMT_RGBA8888 ? 4 : 2);
    uint8_t *data = malloc(bytes);
    TH_CHECK(data != NULL);
    if (!data) return;
    for (size_t i = 0; i < bytes; i++) data[i] = (uint8_t)next();
    if (fmt == TPX_FMT_RGBA8888) {
        for (size_t i = 3; i < bytes; i += 4) {
            if (pattern == 0) data[i] = 0;
            else if (pattern == 1) data[i] = 255;
            else if (pattern == 2) data[i] = ((i / 4) % 3 == 0) ? 0 : 1;
        }
    } else if (pattern < 2) memset(data, pattern == 0 ? 0 : 255, bytes);
    tpx_src_t src = {data, bytes, fmt, sw, sh};
    TH_CHECK(pair(&src, dw, dh, 0));
    free(data);
}
static void images(void)
{
    const uint16_t dims[] = {1, 2, 3, 5, 8, 13};
    for (size_t a = 0; a < 6; a++) for (size_t b = 0; b < 6; b++)
        for (size_t c = 0; c < 6; c++) for (size_t d = 0; d < 6; d++)
            for (unsigned f = 0; f < 2; f++) for (unsigned p = 0; p < 4; p++)
                image(dims[a], dims[b], dims[c], dims[d], (tpx_fmt_t)f, p);
    const uint16_t sizes[][4] = {{2047,1,17,3},{1,2047,3,17},{1,1,2047,1},
        {1,1,1,2047},{360,360,180,180},{90,150,180,300},{512,128,128,512},
        {127,129,63,65},{63,65,127,129},{360,360,360,360},{2047,1,2047,2},{360,90,360,180}};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++)
        for (unsigned f = 0; f < 2; f++) for (unsigned p = 0; p < 4; p++)
            image(sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3], (tpx_fmt_t)f, p);
}
static bool axis(uint16_t sn, uint16_t dn)
{
    size_t old_bytes, new_bytes;
    bool ok = rs_axis_equal(sn, dn, &old_bytes, &new_bytes);
    axis_count++; axis_old_bytes += old_bytes; axis_new_bytes += new_bytes;
    ok = ok && new_bytes < old_bytes;
    if (!ok) fprintf(stderr, "axis mismatch: %u -> %u bytes=%zu/%zu\n",
                     sn, dn, new_bytes, old_bytes);
    return ok;
}
static void axes(bool exhaustive)
{
    /* All dimensions against scale boundaries; every pair is opt-in Release. */
    const uint16_t edges[] = {1,2,3,5,7,8,13,16,31,32,63,64,127,128,255,256,
                             359,360,511,512,1023,1024,2046,2047};
    if (exhaustive) {
        for (uint16_t sn = 1; sn <= TPX_MAX_DIM; sn++)
            for (uint16_t dn = 1; dn <= TPX_MAX_DIM; dn++)
                if (!axis(sn, dn)) { TH_CHECK(false); return; }
    } else {
        for (uint16_t n = 1; n <= TPX_MAX_DIM; n++) {
            if (!axis(n, n)) { TH_CHECK(false); return; }
            for (size_t e = 0; e < sizeof(edges)/sizeof(edges[0]); e++) {
                if (!axis(n, edges[e]) || !axis(edges[e], n)) { TH_CHECK(false); return; }
            }
        }
    }
    TH_CHECK(true);
}
static void failures(void)
{
    uint8_t data[3 * 5 * 4] = {0};
    tpx_src_t s = {data, sizeof(data), TPX_FMT_RGBA8888, 3, 5};
    /* One output allocation plus four per axis: reject every allocation point. */
    for (size_t fail = 1; fail <= 9; fail++) TH_CHECK(pair(&s, 7, 2, fail));
    TH_CHECK(pair(&s, 3, 5, 1));
    TH_CHECK(pair(NULL, 1, 1, 0));
    TH_CHECK(pair(&s, 0, 1, 0));
    TH_CHECK(pair(&s, 1, 0, 0));
    TH_CHECK(pair(&s, 2048, 1, 0));
    s.size--; TH_CHECK(pair(&s, 1, 1, 0)); s.size++;
    s.fmt = (tpx_fmt_t)99; TH_CHECK(pair(&s, 1, 1, 0));
    TH_CHECK(rs_actual(&s, 1, 1, NULL, NULL) == TPX_ERR_ARG);
    TH_CHECK(rs_reference(&s, 1, 1, NULL, NULL) == TPX_ERR_ARG);
}
int main(int argc, char **argv)
{
    bool exhaustive = argc == 2 && !strcmp(argv[1], "--exhaustive");
    bool axis_exhaustive = argc == 2 && !strcmp(argv[1], "--axes-exhaustive");
    if (argc > 1 && !exhaustive && !axis_exhaustive) return 2;
    scalars(exhaustive); images(); failures(); axes(axis_exhaustive);
    printf("scalar_inputs=%" PRIu64 " image_pairs=%" PRIu64 " compared_bytes=%" PRIu64 "\n",
           scalar_count, image_count, image_bytes);
    printf("axis_pairs=%" PRIu64 " old_requested_bytes=%" PRIu64 " new_requested_bytes=%" PRIu64 "\n",
           axis_count, axis_old_bytes, axis_new_bytes);
    return th_report("test_resample");
}
