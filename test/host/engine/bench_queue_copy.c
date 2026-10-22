#define _POSIX_C_SOURCE 200809L
#include "queue_copy_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 7u
#define ITERATIONS 1000000u
typedef uint32_t (*copy_fn)(qc_case_t *, const char *, uint32_t);
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint64_t measure(copy_fn fn, qc_case_t *c, const char *cmd, unsigned count, uint32_t *sum)
{
    uint32_t s = 0;
    unsigned i;
    uint64_t start = ns();
    for (i = 0; i < count; i++) s += fn(c, cmd, i);
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
    static const unsigned lengths[] = {0, 1, 4, 6, 8, 12, 16, 18, 19, 20, 64, 255};
    unsigned n, head, sample;
    puts("head,length,iterations,format_ns,copy_ns,ratio,checksum");
    for (head = 0; head < 24; head += 23) {
        for (n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
            unsigned len = lengths[n], i;
            char *cmd = malloc(len + 1);
            qc_case_t *c = qc_create(head, 0, 73);
            uint64_t old[SAMPLES], copy[SAMPLES];
            uint32_t x, y;
            if (!cmd || !c) abort();
            for (i = 0; i < len; i++) cmd[i] = "0123456789ABCDEF"[i % 16];
            cmd[len] = 0;
            if (!qc_push(c, 0, cmd, 0) || !qc_pop(c)) abort();
            measure(qc_format, c, cmd, 10000, &x);
            measure(qc_copy, c, cmd, 10000, &y);
            if (x != y) abort();
            for (sample = 0; sample < SAMPLES; sample++) {
                if (sample & 1) {
                    copy[sample] = measure(qc_copy, c, cmd, ITERATIONS, &y);
                    old[sample] = measure(qc_format, c, cmd, ITERATIONS, &x);
                } else {
                    old[sample] = measure(qc_format, c, cmd, ITERATIONS, &x);
                    copy[sample] = measure(qc_copy, c, cmd, ITERATIONS, &y);
                }
                if (x != y) abort();
            }
            qsort(old, SAMPLES, sizeof(old[0]), cmp);
            qsort(copy, SAMPLES, sizeof(copy[0]), cmp);
            printf("%u,%u,%u,%.2f,%.2f,%.3f,%u\n", head, len, ITERATIONS,
                   (double)old[SAMPLES / 2] / ITERATIONS,
                   (double)copy[SAMPLES / 2] / ITERATIONS,
                   (double)old[SAMPLES / 2] / (double)copy[SAMPLES / 2], x);
            free(cmd); qc_destroy(c);
        }
    }
    return EXIT_SUCCESS;
}
