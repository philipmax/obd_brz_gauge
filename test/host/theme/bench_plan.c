/* Actual image planner: old per-pixel circle test versus row clipping.
 * Includes alpha bounds/classification, excludes rendering and allocation. */
#define _POSIX_C_SOURCE 200809L
#include "plan_reference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REPEATS 7u
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x > y ? 1 : x < y ? -1 : 0;
}
typedef tpx_err_t (*plan_fn)(const tpx_src_t *, const tpx_opts_t *, tpx_plan_t *);
static uint64_t measure(plan_fn fn, const tpx_src_t *s, const tpx_opts_t *o, unsigned iterations, uint64_t *sum)
{
    uint64_t checksum = 0, start = ns();
    for (unsigned i = 0; i < iterations; i++) {
        tpx_plan_t p;
        if (fn(s, o, &p) != TPX_OK) abort();
        checksum += p.size + p.w + p.h + (uint32_t)p.ox + (uint32_t)p.oy + p.cf;
    }
    uint64_t elapsed = ns() - start;
    *sum = checksum;
    return elapsed;
}
int main(void)
{
    static const char *const patterns[] = {"opaque", "transparent", "mixed", "rim"};
    static const unsigned sizes[] = {16, 64, 360};
    uint8_t *pixels = malloc(360u * 360u * 4u);
    if (!pixels) return 1;
    puts("size,pattern,round,placement,crop,iterations,per_pixel_ns,row_span_ns,ratio,checksum");
    for (unsigned d = 0; d < sizeof(sizes) / sizeof(sizes[0]); d++) {
        unsigned w = sizes[d];
        unsigned iterations = 20000000u / (w * w);
        if (iterations < 200u) iterations = 200u;
        if (iterations > 20000u) iterations = 20000u;
        tpx_src_t s = {pixels, (size_t)w * w * 4u, TPX_FMT_RGBA8888, (uint16_t)w, (uint16_t)w};
        for (unsigned pattern = 0; pattern < 4; pattern++) {
            for (unsigned y = 0; y < w; y++) for (unsigned x = 0; x < w; x++) {
                unsigned i = y * w + x;
                pixels[i * 4u] = (uint8_t)i;
                pixels[i * 4u + 1u] = (uint8_t)(i >> 8);
                pixels[i * 4u + 2u] = (uint8_t)(i >> 3);
                pixels[i * 4u + 3u] = pattern == 0 ? 255 : pattern == 1 ? 0 :
                    pattern == 2 ? (i % 7 == 0 ? 128 : i % 3 == 0 ? 0 : 255) :
                    x < 4 || y < 4 || x >= w - 4 || y >= w - 4 ? 255 : 0;
            }
            for (unsigned round = 0; round < 2; round++)
            for (unsigned placement = 0; placement < (round ? 3u : 1u); placement++)
            for (unsigned crop = 0; crop < 3; crop++) {
                int32_t pos = placement == 0 ? 0 : placement == 1 ? (360 - (int32_t)w) / 2 : -(int32_t)w;
                tpx_opts_t o = {0, 0, (int32_t)w, (int32_t)w, round != 0, pos, pos, (uint8_t)crop};
                uint64_t old[REPEATS], new[REPEATS], a = 0, b = 0;
                for (unsigned i = 0; i < 20; i++) { tpx_plan_t p; tpx_plan_reference(&s, &o, &p); tpx_plan(&s, &o, &p); }
                for (unsigned r = 0; r < REPEATS; r++) {
                    if (r & 1u) { new[r] = measure(tpx_plan, &s, &o, iterations, &b); old[r] = measure(tpx_plan_reference, &s, &o, iterations, &a); }
                    else { old[r] = measure(tpx_plan_reference, &s, &o, iterations, &a); new[r] = measure(tpx_plan, &s, &o, iterations, &b); }
                    if (a != b) abort();
                }
                qsort(old, REPEATS, sizeof(old[0]), cmp); qsort(new, REPEATS, sizeof(new[0]), cmp);
                printf("%u,%s,%u,%u,%u,%u,%.2f,%.2f,%.3f,%llu\n", w, patterns[pattern], round, placement, crop, iterations,
                    (double)old[REPEATS / 2] / iterations, (double)new[REPEATS / 2] / iterations,
                    (double)old[REPEATS / 2] / (double)new[REPEATS / 2], (unsigned long long)a);
            }
        }
    }
    free(pixels);
    return 0;
}
