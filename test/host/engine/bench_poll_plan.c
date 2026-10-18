#define _POSIX_C_SOURCE 200809L
#include "poll_plan_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 7u
#define ITERATIONS 200000u

typedef uint32_t (*plan_fn)(pp_case_t *, uint32_t);

static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint64_t measure(plan_fn fn, pp_case_t *c, unsigned count, uint32_t *sum)
{
    uint32_t s = 0;
    unsigned i;
    uint64_t start = ns();
    for (i = 0; i < count; i++) s += fn(c, i);
    *sum = s;
    return ns() - start;
}
static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
int main(void)
{
    static const unsigned counts[] = {0, 1, 2, 9, 16, 32, 48};
    static const char *const names[] = {"unique", "shared", "paired", "mixed_refs"};
    unsigned n, pattern, sample;
    puts("signals,pattern,iterations,linear_ns,mask_ns,ratio,checksum");
    for (n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
        for (pattern = 0; pattern < 4; pattern++) {
            pp_case_t *a = pp_create(counts[n], pattern, 73, false);
            pp_case_t *b = pp_create(counts[n], pattern, 73, false);
            uint64_t old[SAMPLES], mask[SAMPLES];
            uint32_t x, y;
            if (!a || !b || !pp_verify(a, 0) || !pp_verify(b, 0)) abort();
            measure(pp_linear, a, 10000, &x);
            measure(pp_mask, b, 10000, &y);
            if (x != y) abort();
            for (sample = 0; sample < SAMPLES; sample++) {
                if (sample & 1) {
                    mask[sample] = measure(pp_mask, b, ITERATIONS, &y);
                    old[sample] = measure(pp_linear, a, ITERATIONS, &x);
                } else {
                    old[sample] = measure(pp_linear, a, ITERATIONS, &x);
                    mask[sample] = measure(pp_mask, b, ITERATIONS, &y);
                }
                if (x != y) abort();
            }
            qsort(old, SAMPLES, sizeof(old[0]), cmp);
            qsort(mask, SAMPLES, sizeof(mask[0]), cmp);
            printf("%u,%s,%u,%.2f,%.2f,%.3f,%u\n", counts[n], names[pattern], ITERATIONS,
                   (double)old[SAMPLES / 2] / ITERATIONS,
                   (double)mask[SAMPLES / 2] / ITERATIONS,
                   (double)old[SAMPLES / 2] / (double)mask[SAMPLES / 2], x);
            pp_destroy(a); pp_destroy(b);
        }
    }
    return 0;
}
