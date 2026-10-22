#define _POSIX_C_SOURCE 200809L
#include "write_rounding_probe.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define SAMPLES 7u
#define ITERATIONS 1000000u
typedef bool (*round_fn)(float, int32_t *);
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint64_t measure(round_fn fn, const float *inputs, unsigned n, uint64_t *checksum)
{
    unsigned i; uint64_t sum = 0, start = ns();
    for (i = 0; i < n; i++) {
        int32_t value = 123456789;
        bool valid = fn(inputs[i & 255u], &value);
        sum += (uint32_t)value + (uint64_t)valid;
    }
    *checksum = sum;
    return ns() - start;
}
static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
int main(void)
{
    static const char *names[] = {"rpm_integer", "temperature_fraction", "half_neighbors",
        "large_identity", "mixed_patterns", "invalid"};
    float inputs[256]; unsigned c, i, s; uint32_t seed = UINT32_C(0x273ed915);
    puts("case,iterations,double_ns,production_ns,double_to_production,checksum");
    for (c = 0; c < sizeof(names)/sizeof(names[0]); c++) {
        uint64_t old[SAMPLES], candidate[SAMPLES], a, b;
        for (i = 0; i < 256; i++) {
            switch (c) {
            case 0: inputs[i] = (float)(i * 31u); break;
            case 1: inputs[i] = (float)((int)i - 40) / 3.0f; break;
            case 2:
                inputs[i] = (float)((int)i - 128) + 0.5f;
                if (i % 3 != 0) inputs[i] = nextafterf(inputs[i], i % 3 == 1 ? -INFINITY : INFINITY);
                break;
            case 3: inputs[i] = (float)(i * 65537u + 8388609u); break;
            case 4:
                seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
                memcpy(&inputs[i], &seed, sizeof(seed)); break;
            default: inputs[i] = i & 1 ? NAN : i & 2 ? -INFINITY : 2147483648.0f; break;
            }
            { int32_t x = 1, y = 1;
              if (wr_reference(inputs[i], &x) != wr_identity(inputs[i], &y) || x != y) abort(); }
        }
        measure(wr_reference, inputs, 10000, &a); measure(wr_identity, inputs, 10000, &b);
        if (a != b) abort();
        for (s = 0; s < SAMPLES; s++) {
            if (s & 1u) {
                candidate[s] = measure(wr_identity, inputs, ITERATIONS, &b);
                old[s] = measure(wr_reference, inputs, ITERATIONS, &a);
            } else {
                old[s] = measure(wr_reference, inputs, ITERATIONS, &a);
                candidate[s] = measure(wr_identity, inputs, ITERATIONS, &b);
            }
            if (a != b) abort();
        }
        qsort(old, SAMPLES, sizeof(old[0]), cmp); qsort(candidate, SAMPLES, sizeof(candidate[0]), cmp);
        printf("%s,%u,%.2f,%.2f,%.3f,%llu\n", names[c], ITERATIONS,
               (double)old[SAMPLES/2]/ITERATIONS, (double)candidate[SAMPLES/2]/ITERATIONS,
               (double)old[SAMPLES/2]/(double)candidate[SAMPLES/2], (unsigned long long)a);
    }
    return 0;
}
