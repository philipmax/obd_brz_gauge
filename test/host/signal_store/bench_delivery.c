/* Actual delivered-unit formulas, independent TUs, no LTO, no store locks. */
#define _POSIX_C_SOURCE 200809L
#include "delivery_reference.h"
#include "app_obd_dsp/signal_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 7u
#define ITERATIONS 1000000u
#define INPUTS 256u
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint64_t measure(double (*fn)(int, int32_t), int slot, const int32_t *values,
                        unsigned iterations, double *checksum)
{
    double sum = 0;
    uint64_t start = ns();
    for (unsigned i = 0; i < iterations; i++) sum += fn(slot, values[i % INPUTS]);
    uint64_t elapsed = ns() - start;
    *checksum = sum;
    return elapsed;
}
static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
int main(void)
{
    static const struct { const char *name; int slot; int32_t limit; int negative; } cases[] = {
        {"oil_pressure", SIG_OBD_OIL_PRESSURE, 2000, 0},
        {"battery_voltage", SIG_OBD_BATTERY_VOLTAGE, 32000, 0},
        {"rpm_identity", SIG_OBD_RPM, 20000, 0},
        {"veh_identity", SIG_VEH_BASE, 20000, 0},
        {"oil_pressure_negative", SIG_OBD_OIL_PRESSURE, 2000, 1},
        {"battery_voltage_negative", SIG_OBD_BATTERY_VOLTAGE, 32000, 1}
    };
    puts("case,iterations,floating_ns,integer_ns,ratio,checksum");
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        int32_t values[INPUTS];
        for (unsigned i = 0; i < INPUTS; i++) {
            values[i] = (int32_t)((i * 193u + 17u) % ((unsigned)cases[c].limit + 1u));
            if (cases[c].negative) values[i] = -values[i] - 1;
        }
        uint64_t floating[SAMPLES], integer[SAMPLES];
        double a = 0, b = 0;
        measure(delivery_reference, cases[c].slot, values, 20000, &a);
        measure(delivery_candidate, cases[c].slot, values, 20000, &b);
        for (unsigned s = 0; s < SAMPLES; s++) {
            if (s & 1u) {
                integer[s] = measure(delivery_candidate, cases[c].slot, values, ITERATIONS, &b);
                floating[s] = measure(delivery_reference, cases[c].slot, values, ITERATIONS, &a);
            } else {
                floating[s] = measure(delivery_reference, cases[c].slot, values, ITERATIONS, &a);
                integer[s] = measure(delivery_candidate, cases[c].slot, values, ITERATIONS, &b);
            }
            if (a != b) abort();
        }
        qsort(floating, SAMPLES, sizeof(floating[0]), cmp); qsort(integer, SAMPLES, sizeof(integer[0]), cmp);
        printf("%s,%u,%.2f,%.2f,%.3f,%.0f\n", cases[c].name, ITERATIONS,
            (double)floating[SAMPLES / 2] / ITERATIONS, (double)integer[SAMPLES / 2] / ITERATIONS,
            (double)floating[SAMPLES / 2] / (double)integer[SAMPLES / 2], a);
    }
    return 0;
}
