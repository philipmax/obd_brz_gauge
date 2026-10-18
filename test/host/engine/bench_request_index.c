#define _POSIX_C_SOURCE 200809L
#include "request_index_selection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SAMPLES       7u
#define ITERATIONS   1000000u
#define QUERY_COUNT      256u
#define WARMUP        20000u

typedef uint32_t (*dispatch_fn)(const ri_case_t *, uint8_t);

typedef struct {
    const char *name;
    uint8_t n_sig;
    uint8_t n_src;
    uint8_t n_req;
    uint8_t driven;
    uint8_t sources_per_signal;
} case_def_t;

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static uint64_t measure(dispatch_fn fn, const ri_case_t *c, const uint8_t *queries,
                        uint32_t iterations, uint32_t *checksum)
{
    uint32_t sum = 0;
    uint64_t start = now_ns();
    uint32_t i;
    for (i = 0; i < iterations; i++) {
        sum += fn(c, queries[i % QUERY_COUNT]);
    }
    *checksum = sum;
    return now_ns() - start;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void make_case(ri_case_t *c, const case_def_t *d)
{
    uint8_t i;
    uint16_t source = 0;

    memset(c, 0, sizeof(*c));
    c->n_sig = d->n_sig;
    c->n_req = d->n_req;
    for (i = 0; i < c->n_sig; i++) {
        ri_signal_t *sig = &c->sig[i];
        uint8_t k;
        sig->first = source;
        sig->count = d->sources_per_signal;
        sig->cur = d->sources_per_signal > 1u ? (uint8_t)(i & 1u) : 0u; /* exercise valid current-source offsets */
        sig->driven = i < d->driven;
        for (k = 0; k < sig->count; k++) {
            ri_source_t *src = &c->src[source++];
            src->sig = i;
            src->ref = (uint8_t)((i * 11u + k * 7u + (i >> 2)) % c->n_req);
            src->kind = (k == 1u && (i % 5u) == 0u) ? 2u : 1u;
        }
    }
    c->n_src = (uint8_t)source;
    ri_build_index(c);
}

static void run_case(const case_def_t *d, const uint8_t *queries)
{
    ri_case_t c;
    uint64_t linear[SAMPLES], indexed[SAMPLES];
    uint32_t a = 0, b = 0;
    uint32_t i;

    make_case(&c, d);
    for (i = 0; i < QUERY_COUNT; i++) {
        a += ri_dispatch_linear(&c, queries[i]);
        b += ri_dispatch_indexed(&c, queries[i]);
    }
    if (a != b) {
        fprintf(stderr, "%s checksum mismatch before timing: %u != %u\n", d->name, a, b);
        exit(1);
    }
    (void)measure(ri_dispatch_linear, &c, queries, WARMUP, &a);
    (void)measure(ri_dispatch_indexed, &c, queries, WARMUP, &b);
    for (i = 0; i < SAMPLES; i++) {
        if (i & 1u) {
            indexed[i] = measure(ri_dispatch_indexed, &c, queries, ITERATIONS, &b);
            linear[i] = measure(ri_dispatch_linear, &c, queries, ITERATIONS, &a);
        } else {
            linear[i] = measure(ri_dispatch_linear, &c, queries, ITERATIONS, &a);
            indexed[i] = measure(ri_dispatch_indexed, &c, queries, ITERATIONS, &b);
        }
        if (a != b) {
            fprintf(stderr, "%s checksum mismatch: %u != %u\n", d->name, a, b);
            exit(1);
        }
    }
    qsort(linear, SAMPLES, sizeof(linear[0]), cmp_u64);
    qsort(indexed, SAMPLES, sizeof(indexed[0]), cmp_u64);
    printf("%s,%u,%u,%u,%u,%u,%.2f,%.2f,%.3f,%u\n", d->name,
           c.n_sig, c.n_src, c.n_req, QUERY_COUNT, ITERATIONS,
           (double)linear[SAMPLES / 2] / ITERATIONS,
           (double)indexed[SAMPLES / 2] / ITERATIONS,
           (double)linear[SAMPLES / 2] / indexed[SAMPLES / 2], a);
}

int main(void)
{
    static const case_def_t cases[] = {
        {"catalog_like", 12, 24, 12, 12, 2},
        {"mixed_max",     48, 96, 32, 32, 2},
        {"sparse_max",    48, 48, 32, 16, 1},
        {"can_heavy",     48, 96, 32, 16, 2},
    };
    uint8_t queries[QUERY_COUNT];
    uint32_t i;

    for (i = 0; i < QUERY_COUNT; i++) queries[i] = (uint8_t)((i * 13u + (i >> 3)) % 32u);
    puts("case,signals,sources,requests,queries,iterations,linear_ns,indexed_ns,ratio,checksum");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) run_case(&cases[i], queries);
    return 0;
}
