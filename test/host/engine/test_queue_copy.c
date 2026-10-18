#include "queue_copy_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng(uint32_t *s)
{
    *s = *s * UINT32_C(1664525) + UINT32_C(1013904223);
    return *s;
}
int main(void)
{
    static const unsigned lengths[] = {0, 1, 2, 4, 8, 12, 18, 19, 20, 21, 64, 255};
    static const uint16_t delays[] = {0, 1, 30, 150, 1000, UINT16_MAX};
    unsigned head, count, n, kind, stream, step, checks = 0;
    /* Exact allocations give ASAN a boundary immediately after the terminator. */
    for (head = 0; head < 24; head++) for (count = 0; count <= 24; count++) {
        for (n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
            unsigned len = lengths[n], i;
            char *cmd = malloc(len + 1);
            if (!cmd) abort();
            for (i = 0; i < len; i++) cmd[i] = (char)(1 + (i * 73u + n) % 255u);
            cmd[len] = 0;
            for (kind = 0; kind < 8; kind++) {
                qc_case_t *c = qc_create(head, count, n + head * 31 + count * 97 + kind);
                if (!c || !qc_push(c, (uint8_t)kind, cmd, delays[(n + kind) % 6])) {
                    fprintf(stderr, "push mismatch: head=%u count=%u len=%u kind=%u\n",
                            head, count, len, kind); return EXIT_FAILURE;
                }
                checks++;
                if (qc_count(c)) {
                    if (!qc_pop(c)) return EXIT_FAILURE;
                    checks++;
                }
                qc_destroy(c);
            }
            free(cmd);
        }
    }
    /* Stateful fill/drop/drain/rewrap sequences, including embedded NULs. */
    for (stream = 0; stream < 256; stream++) {
        uint32_t seed = stream + 1;
        qc_case_t *c = qc_create(stream % 24, stream % 25, seed);
        if (!c) abort();
        for (step = 0; step < 512; step++) {
            if (qc_count(c) && rng(&seed) % 3 == 0) {
                if (!qc_pop(c)) return EXIT_FAILURE;
            } else {
                unsigned len = rng(&seed) % 256u, i;
                char *cmd = malloc(len + 1);
                uint8_t k = (uint8_t)rng(&seed);
                uint16_t delay = (uint16_t)rng(&seed);
                if (!cmd) abort();
                for (i = 0; i < len; i++) cmd[i] = (char)(rng(&seed) >> 24);
                cmd[len] = 0;
                if (!qc_push(c, k, cmd, delay)) return EXIT_FAILURE;
                free(cmd);
            }
            checks++;
        }
        qc_destroy(c);
    }
    printf("queue copy: %u complete-state/step/log comparisons passed\n", checks);
    return EXIT_SUCCESS;
}
