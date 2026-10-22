#include "poll_plan_probe.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    unsigned n, pattern, mixed, round;
    unsigned checks = 0;
    static const uint32_t times[] = {
        0, 1, 99, 100, 499, 500, 1000, UINT32_MAX - 500,
        UINT32_MAX - 1, UINT32_MAX, 0, 1, 500
    };
    for (n = 0; n <= 48; n++) {
        for (pattern = 0; pattern < 4; pattern++) {
            for (mixed = 0; mixed < 2; mixed++) {
                pp_case_t *c = pp_create(n, pattern, n * 97u + pattern + 1, mixed != 0);
                if (!c) return EXIT_FAILURE;
                for (round = 0; round < 256; round++) {
                    uint32_t now = round < sizeof(times) / sizeof(times[0])
                        ? times[round] : (round - 13) * 37u;
                    pp_transition(c, round * 773u + n + 1);
                    if (!pp_verify(c, now)) {
                        fprintf(stderr, "poll state mismatch n=%u pattern=%u mixed=%u round=%u\n",
                                n, pattern, mixed, round);
                        return EXIT_FAILURE;
                    }
                    checks++;
                }
                pp_destroy(c);
            }
        }
    }
    printf("poll planner: %u full-state/slot comparisons passed\n", checks);
    return EXIT_SUCCESS;
}
