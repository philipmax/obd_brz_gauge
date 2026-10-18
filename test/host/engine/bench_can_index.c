/* CAN rule selection only: no ELM parsing, RPN, callbacks or RTOS costs. */
#define _POSIX_C_SOURCE 200809L
#include "can_index_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 7u
#define QUERIES 256u
#define ITERATIONS 500000u

typedef uint64_t (*visit_fn)(const vp_can_rule_t *, const uint8_t *, uint16_t, uint32_t);
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint64_t measure(visit_fn fn, const vp_can_rule_t *r, const uint8_t *order,
                        uint16_t n, const uint32_t *queries, unsigned iterations, uint64_t *checksum)
{
    uint64_t sum = 0, start = ns();
    for (unsigned i = 0; i < iterations; i++) sum += fn(r, order, n, queries[i % QUERIES]);
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
    static const uint16_t counts[] = {0, 1, 2, 4, 8, 16};
    static const char *const patterns[] = {"unique", "paired", "same"};
    static const char *const streams[] = {"miss", "mixed", "hit"};
    puts("rules,pattern,stream,iterations,linear_ns,indexed_ns,ratio,checksum");
    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        uint16_t n = counts[c];
        for (unsigned pattern = 0; pattern < 3; pattern++) {
            vp_can_rule_t rules[VP_MAX_CAN_RULES] = {{0}};
            uint8_t order[VP_MAX_CAN_RULES] = {0};
            for (uint16_t i = 0; i < n; i++) {
                uint32_t rank = pattern == 0 ? (uint32_t)(n - 1 - i) : pattern == 1 ? (uint32_t)((n - 1 - i) / 2) : 0;
                rules[i].can_id = 0x400u + rank * 2u;
                rules[i].src = i;
            }
            vp_can_index_build(rules, n, order);
            for (unsigned stream = 0; stream < 3; stream++) {
                uint32_t queries[QUERIES];
                for (unsigned q = 0; q < QUERIES; q++) {
                    uint32_t matched = n ? rules[(q * 73u) % n].can_id : 0x400u;
                    queries[q] = stream == 0 || (stream == 1 && q % 4u) ? matched + 1u : matched;
                }
                uint64_t linear[SAMPLES], indexed[SAMPLES], a = 0, b = 0;
                for (unsigned q = 0; q < QUERIES; q++) {
                    if (visit_linear(rules, order, n, queries[q]) != visit_indexed(rules, order, n, queries[q])) abort();
                }
                measure(visit_linear, rules, order, n, queries, 20000u, &a);
                measure(visit_indexed, rules, order, n, queries, 20000u, &b);
                for (unsigned s = 0; s < SAMPLES; s++) {
                    if (s & 1u) {
                        indexed[s] = measure(visit_indexed, rules, order, n, queries, ITERATIONS, &b);
                        linear[s] = measure(visit_linear, rules, order, n, queries, ITERATIONS, &a);
                    } else {
                        linear[s] = measure(visit_linear, rules, order, n, queries, ITERATIONS, &a);
                        indexed[s] = measure(visit_indexed, rules, order, n, queries, ITERATIONS, &b);
                    }
                    if (a != b) abort();
                }
                qsort(linear, SAMPLES, sizeof(linear[0]), cmp);
                qsort(indexed, SAMPLES, sizeof(indexed[0]), cmp);
                printf("%u,%s,%s,%u,%.2f,%.2f,%.3f,%llu\n", n, patterns[pattern], streams[stream], ITERATIONS,
                    (double)linear[SAMPLES / 2] / ITERATIONS, (double)indexed[SAMPLES / 2] / ITERATIONS,
                    (double)linear[SAMPLES / 2] / (double)indexed[SAMPLES / 2], (unsigned long long)a);
            }
        }
    }
    return 0;
}
