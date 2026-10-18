/* Independent double oracle for Binary32 rounding + real pipeline replay. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "write_rounding_probe.h"
static uint64_t cases, pipeline_cases;
static float from_bits(uint32_t bits) { float f; memcpy(&f, &bits, sizeof(f)); return f; }
static void check(float value)
{
    int32_t a = 123456789, b = a;
    bool x = wr_identity(value, &a), y = wr_reference(value, &b);
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits)); cases++;
    if (x != y || a != b) {
        fprintf(stderr, "round bits=%08x: actual=%d/%d expected=%d/%d\n", bits, x, a, y, b);
        exit(1);
    }
}
int main(int argc, char **argv)
{
    uint32_t seed = UINT32_C(0x1637ca89);
    unsigned j, k;
    int i;
    static const float edges[] = {
        0.0f, -0.0f, 0.5f, -0.5f, 1.5f, -1.5f, 8388609.0f, -8388609.0f,
        16777215.0f, -16777215.0f, 2147483648.0f, -2147483648.0f,
        INFINITY, -INFINITY, NAN
    };
    for (j = 0; j < sizeof(edges)/sizeof(edges[0]); j++) {
        check(edges[j]); check(nextafterf(edges[j], INFINITY)); check(nextafterf(edges[j], -INFINITY));
    }
    for (i = -32768; i <= 32768; i++) {
        float tie = (float)i + 0.5f;
        check(tie); check(nextafterf(tie, -INFINITY)); check(nextafterf(tie, INFINITY));
    }
    for (j = 0; j < 256; j++) {
        uint32_t center = j << 23;
        for (k = 0; k < 1024; k++) {
            check(from_bits(center + k)); check(from_bits(center - k));
        }
    }
    for (j = 0; j < 1000000; j++) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223); check(from_bits(seed));
    }
    for (j = 0; j < 1040; j++) {
        static const unsigned indexes[] = {0, 31, 32, 47};
        for (k = 0; k < 32; k++) {
            float value;
            if (k < 15) value = edges[k];
            else if (k == 15) value = nextafterf(0.5f, 0.0f);
            else if (k == 16) value = nextafterf(-0.5f, -INFINITY);
            else if (k < 22) value = (float)((int)k - 18) + 0.5f;
            else { seed = seed * UINT32_C(1664525) + UINT32_C(1013904223); value = from_bits(seed); }
            if (!wr_compare(value, j, indexes[k % 4])) {
                fprintf(stderr, "pipeline config=%u index=%u value=%a failed\n", j, indexes[k%4], (double)value);
                return 1;
            }
            pipeline_cases++;
        }
    }
    if (argc == 2 && !strcmp(argv[1], "--exhaustive")) {
        uint64_t bits;
        for (bits = 0; bits <= UINT32_MAX; bits++) {
            check(from_bits((uint32_t)bits));
            if ((bits & UINT32_C(0x0fffffff)) == UINT32_C(0x0fffffff)) {
                printf("checked %llu exhaustive bit patterns\n", (unsigned long long)(bits + 1)); fflush(stdout);
            }
        }
    } else if (argc != 1) return 2;
    printf("PASS: %llu rounding patterns; %llu complete pipeline state/callback comparisons\n",
           (unsigned long long)cases, (unsigned long long)pipeline_cases);
    return 0;
}
