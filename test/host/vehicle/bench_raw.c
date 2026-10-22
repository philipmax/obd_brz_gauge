/* Explicit opt-in benchmark; elapsed times are not CI pass/fail assertions.
 * Both implementations use runtime inputs, one indirect call per extraction,
 * identical payloads, and matching checksums. Build without sanitizers. */
#define _POSIX_C_SOURCE 200809L
#include "vp_rpn.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 7
#define ITERATIONS 1000000u

typedef bool (*extract_fn)(const uint8_t *, size_t, uint16_t, uint8_t, uint8_t, int64_t *);

/* Previous production implementation, kept as the comparison baseline. */
static bool bitwise(const uint8_t *data, size_t len, uint16_t start,
                    uint8_t bits, uint8_t flags, int64_t *out)
{
    uint32_t v = 0;
    if (!data || !out || bits == 0 || bits > 32) return false;
    if ((uint64_t)start + bits > (uint64_t)len * 8u) return false;
    if (flags & VP_RAW_BE) {
        for (uint8_t i = 0; i < bits; i++) {
            uint32_t bit = (uint32_t)start + i;
            v = (v << 1) | ((data[bit >> 3] >> (7u - (bit & 7u))) & 1u);
        }
    } else {
        for (uint8_t i = 0; i < bits; i++) {
            uint32_t bit = (uint32_t)start + i;
            v |= (uint32_t)((data[bit >> 3] >> (bit & 7u)) & 1u) << i;
        }
    }
    *out = ((flags & VP_RAW_SIGNED) && (v & (1u << (bits - 1))))
         ? (int64_t)v - ((int64_t)1 << bits) : (int64_t)v;
    return true;
}

static double now_ns(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) abort();
    return (double)time.tv_sec * 1e9 + time.tv_nsec;
}

static double run(extract_fn volatile fn, uint16_t start, uint8_t bits,
                  uint8_t flags, uint64_t *checksum)
{
    uint8_t data[256][8];
    uint32_t random = 0x41A37BC9;
    for (unsigned i = 0; i < 256; i++) {
        for (unsigned j = 0; j < 8; j++) {
            random = random * 1664525u + 1013904223u;
            data[i][j] = (uint8_t)(random >> 24);
        }
    }
    uint64_t sum = 0;
    double begin = now_ns();
    for (unsigned i = 0; i < ITERATIONS; i++) {
        int64_t value;
        if (!fn(data[i & 255u], 8, start, bits, flags, &value)) abort();
        sum += (uint64_t)value;
    }
    double elapsed = now_ns() - begin;
    *checksum = sum;
    return elapsed / ITERATIONS;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    static const struct { uint16_t start; uint8_t bits, flags; } cases[] = {
        {16, 1, 0}, {16, 8, 0}, {3, 8, 0}, {16, 16, 0}, {3, 16, 0}, {0, 32, 0}, {3, 32, VP_RAW_SIGNED},
        {16, 1, VP_RAW_BE}, {16, 8, VP_RAW_BE}, {3, 8, VP_RAW_BE}, {16, 16, VP_RAW_BE},
        {3, 16, VP_RAW_BE}, {0, 32, VP_RAW_BE}, {3, 32, VP_RAW_BE | VP_RAW_SIGNED}
    };
    printf("start,bits,endian,signed,bitwise_ns,bytewise_ns,speedup,checksum\n");
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        double old[SAMPLES], optimized[SAMPLES];
        uint64_t a = 0, b = 0;
        /* Warm both paths, then alternate measurement order. */
        run(bitwise, cases[i].start, cases[i].bits, cases[i].flags, &a);
        run(vp_rpn_extract_raw, cases[i].start, cases[i].bits, cases[i].flags, &b);
        for (unsigned j = 0; j < SAMPLES; j++) {
            if (j & 1u) {
                optimized[j] = run(vp_rpn_extract_raw, cases[i].start, cases[i].bits, cases[i].flags, &b);
                old[j] = run(bitwise, cases[i].start, cases[i].bits, cases[i].flags, &a);
            } else {
                old[j] = run(bitwise, cases[i].start, cases[i].bits, cases[i].flags, &a);
                optimized[j] = run(vp_rpn_extract_raw, cases[i].start, cases[i].bits, cases[i].flags, &b);
            }
            if (a != b) { fprintf(stderr, "checksum mismatch\n"); return 1; }
        }
        qsort(old, SAMPLES, sizeof(double), cmp_double);
        qsort(optimized, SAMPLES, sizeof(double), cmp_double);
        double baseline = old[SAMPLES / 2], improved = optimized[SAMPLES / 2];
        printf("%u,%u,%s,%u,%.2f,%.2f,%.2f,%" PRIu64 "\n", cases[i].start, cases[i].bits,
               cases[i].flags & VP_RAW_BE ? "be" : "le", !!(cases[i].flags & VP_RAW_SIGNED),
               baseline, improved, baseline / improved, b);
    }
    return 0;
}
