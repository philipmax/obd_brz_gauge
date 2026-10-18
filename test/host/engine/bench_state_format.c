#define _POSIX_C_SOURCE 200809L
#include "queue_copy_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef EH_STATE_BASELINE
/* Whole-source baseline compiled separately; same opaque state/layout. */
bool qc_baseline_state(qc_case_t *, uint8_t, uint32_t, int16_t);
uint32_t qc_baseline_state_copy(qc_case_t *, uint8_t, uint32_t, int16_t);
#define REFERENCE_STATE qc_baseline_state_copy
#else
#define REFERENCE_STATE qc_state_format
#endif

#define SAMPLES 7u
#define ITERATIONS 1000000u
typedef uint32_t (*state_fn)(qc_case_t *, uint8_t, uint32_t, int16_t);
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint64_t measure(state_fn fn, qc_case_t *c, uint8_t st, uint32_t val, unsigned count, uint32_t *sum)
{
    uint32_t s = 0;
    unsigned i;
    uint64_t start = ns();
    for (i = 0; i < count; i++) s += fn(c, st, val, (int16_t)(i % 32u));
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
    static const struct { const char *name; uint8_t st; uint32_t val; } cases[] = {
        {"hdr11", 0, 0x7E0}, {"hdr29", 0, 0x98DA10F1}, {"hdr29_zero", 0, 0x80000000},
        {"rx_off", 1, 0}, {"rx11", 1, 0x7E8}, {"rx29", 1, 0x98DAF110},
        {"cea_off", 2, 256}, {"cea_zero", 2, 0}, {"cea_f1", 2, 0xF1},
        {"caf0", 3, 1}, {"caf1", 3, 0}, {"caf_default", 255, UINT32_MAX}
    };
    unsigned n, head, sample;
#ifdef EH_STATE_BASELINE
    puts("head,case,iterations,baseline_ns,production_ns,baseline_to_production,checksum");
#else
    puts("head,case,iterations,reference_ns,production_ns,reference_to_production,checksum");
#endif
    for (head = 0; head < 24; head += 23) for (n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        qc_case_t *c = qc_create(head, 0, 73);
        uint64_t old[SAMPLES], direct[SAMPLES];
        uint32_t x, y;
        if (!c || !qc_state(c, cases[n].st, cases[n].val, -1) || !qc_pop(c)) abort();
#ifdef EH_STATE_BASELINE
        if (!qc_baseline_state(c, cases[n].st, cases[n].val, -1) || !qc_pop(c)) abort();
#endif
        measure(REFERENCE_STATE, c, cases[n].st, cases[n].val, 10000, &x);
        measure(qc_state_copy, c, cases[n].st, cases[n].val, 10000, &y);
        if (x != y) abort();
        for (sample = 0; sample < SAMPLES; sample++) {
            if (sample & 1) {
                direct[sample] = measure(qc_state_copy, c, cases[n].st, cases[n].val, ITERATIONS, &y);
                old[sample] = measure(REFERENCE_STATE, c, cases[n].st, cases[n].val, ITERATIONS, &x);
            } else {
                old[sample] = measure(REFERENCE_STATE, c, cases[n].st, cases[n].val, ITERATIONS, &x);
                direct[sample] = measure(qc_state_copy, c, cases[n].st, cases[n].val, ITERATIONS, &y);
            }
            if (x != y) abort();
        }
        qsort(old, SAMPLES, sizeof(old[0]), cmp);
        qsort(direct, SAMPLES, sizeof(direct[0]), cmp);
        printf("%u,%s,%u,%.2f,%.2f,%.3f,%u\n", head, cases[n].name, ITERATIONS,
               (double)old[SAMPLES / 2] / ITERATIONS,
               (double)direct[SAMPLES / 2] / ITERATIONS,
               (double)old[SAMPLES / 2] / (double)direct[SAMPLES / 2], x);
        qc_destroy(c);
    }
    return EXIT_SUCCESS;
}
