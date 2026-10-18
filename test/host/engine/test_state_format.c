#include "queue_copy_probe.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static uint32_t rng(uint32_t *s)
{
    *s = *s * UINT32_C(1664525) + UINT32_C(1013904223);
    return *s;
}
static void one(unsigned head, unsigned count, uint8_t st, uint32_t val, int16_t req)
{
    qc_case_t *c = qc_create(head, count, val ^ st ^ head);
    if (!c || !qc_state(c, st, val, req)) {
        fprintf(stderr, "state mismatch: head=%u count=%u st=%u val=%08X req=%d\n",
                head, count, st, (unsigned)val, req);
        exit(EXIT_FAILURE);
    }
    checks++;
    if (qc_count(c)) {
        if (!qc_pop(c)) exit(EXIT_FAILURE);
        checks++;
    }
    qc_destroy(c);
}
int main(void)
{
    static const unsigned counts[] = {0, 1, 23, 24};
    static const uint32_t values[] = {
        0, 1, 0x7DF, 0x7E0, 0x7E8, 0x7FF, 0x800, 0x7FFFFFFF,
        0x80000000, 0x80000001, 0x800007FF, 0x81000000,
        0x98DA10F1, 0x98DA18F1, 0x98DAF110, 0x9FFFFFFF, UINT32_MAX
    };
    static const int16_t refs[] = {-1, 0, 31, INT16_MIN, INT16_MAX};
    unsigned head, count, i, st, stream, step;
    /* Every 11-bit value for both header and receive-filter commands. */
    for (i = 0; i < 2048; i++) for (st = 0; st < 2; st++) {
        for (count = 0; count < sizeof(counts) / sizeof(counts[0]); count++)
            one(i % 24, counts[count], (uint8_t)st, i, refs[i % 5]);
    }
    /* Leading zeros, flag/mask boundaries, known extended IDs, full/drop/wrap. */
    for (head = 0; head < 24; head++) for (count = 0; count < sizeof(counts) / sizeof(counts[0]); count++) {
        for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) for (st = 0; st < 4; st++)
            one(head, counts[count], (uint8_t)st, values[i], refs[(i + st) % 5]);
    }
    /* All address bytes plus off sentinel. Every unknown state uses CAF. */
    for (i = 0; i <= 256; i++) one(i % 24, i % 25, 2, i, refs[i % 5]);
    for (st = 3; st <= 255; st++) for (i = 0; i < 3; i++)
        one(st % 24, i == 2 ? 24 : i, (uint8_t)st, i ? UINT32_MAX : 0, refs[st % 5]);
    /* Stateful randomized transitions exercise all input bits and metadata. */
    for (stream = 0; stream < 256; stream++) {
        uint32_t seed = stream + 1;
        qc_case_t *c = qc_create(stream % 24, stream % 25, seed);
        if (!c) abort();
        for (step = 0; step < 512; step++) {
            if (qc_count(c) && rng(&seed) % 3 == 0) {
                if (!qc_pop(c)) exit(EXIT_FAILURE);
            } else {
                uint8_t state = (uint8_t)(rng(&seed) % 5u);
                uint32_t val = rng(&seed);
                int16_t ref = (int16_t)rng(&seed);
                if (!qc_state(c, state, val, ref)) exit(EXIT_FAILURE);
            }
            checks++;
        }
        qc_destroy(c);
    }
    printf("state formatting: %u complete-state/step/log comparisons passed\n", checks);
    return EXIT_SUCCESS;
}
