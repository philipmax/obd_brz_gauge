#include "app_obd_dsp/perf_hist.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)

static void test_boundaries(void)
{
    perf_hist_t h;
    perf_hist_reset(&h);
    CHECK(h.samples == 0 && h.total_us == 0 && h.units == 0);
    CHECK(perf_hist_percentile_upper(&h, 95) == 0);
    CHECK(h.min_us == UINT32_MAX && h.max_us == 0);
    for (unsigned i = 0; i < PERF_HIST_BUCKETS - 1; i++) {
        uint32_t limit = 1u << i;
        uint32_t durations[] = {limit - 1, limit, limit + 1};
        for (unsigned j = 0; j < 3; j++) {
            perf_hist_reset(&h);
            CHECK(perf_hist_add(&h, durations[j], 16384));
            uint32_t bound = perf_hist_percentile_upper(&h, 95);
            CHECK(bound >= durations[j]);
            CHECK(h.min_us == durations[j] && h.max_us == durations[j]);
            CHECK(h.total_us == durations[j] && h.units == 16384);
            unsigned bucket = 0;
            while (bucket < PERF_HIST_BUCKETS - 1 && (1u << bucket) < durations[j]) bucket++;
            for (unsigned k = 0; k < PERF_HIST_BUCKETS; k++) CHECK(h.buckets[k] == (k == bucket));
        }
    }
    perf_hist_reset(&h);
    CHECK(perf_hist_add(&h, UINT32_MAX, UINT32_MAX));
    CHECK(perf_hist_add(&h, UINT32_MAX, UINT32_MAX));
    CHECK(h.total_us == (uint64_t)UINT32_MAX * 2);
    CHECK(h.units == (uint64_t)UINT32_MAX * 2);
    CHECK(perf_hist_percentile_upper(&h, 100) == UINT32_MAX);
    CHECK(perf_hist_percentile_upper(&h, 0) == 0);
    CHECK(perf_hist_percentile_upper(&h, 101) == 0);
    h.samples = UINT32_MAX;
    perf_hist_t before = h;
    CHECK(!perf_hist_add(&h, 1, 1));
    CHECK(memcmp(&h, &before, sizeof(h)) == 0);
}

static void test_percentiles(void)
{
    perf_hist_t h;
    perf_hist_reset(&h);
    for (unsigned i = 0; i < 95; i++) CHECK(perf_hist_add(&h, 10, 1));
    for (unsigned i = 0; i < 5; i++) CHECK(perf_hist_add(&h, 1000, 1));
    CHECK(h.samples == 100 && h.total_us == 5950 && h.units == 100);
    CHECK(h.min_us == 10 && h.max_us == 1000);
    CHECK(perf_hist_percentile_upper(&h, 50) == 16);
    CHECK(perf_hist_percentile_upper(&h, 95) == 16);
    CHECK(perf_hist_percentile_upper(&h, 96) == 1024);
    CHECK(perf_hist_percentile_upper(&h, 100) == 1024);
    perf_hist_reset(&h);
    CHECK(h.samples == 0 && h.total_us == 0 && h.units == 0);
    CHECK(h.min_us == UINT32_MAX && h.max_us == 0);
    for (unsigned i = 0; i < PERF_HIST_BUCKETS; i++) CHECK(h.buckets[i] == 0);
    CHECK(perf_hist_add(&h, 0, 0));
    CHECK(h.samples == 1 && h.total_us == 0 && h.min_us == 0 && h.max_us == 0);
    CHECK(perf_hist_percentile_upper(&h, 95) == 1);
}

int main(void)
{
    test_boundaries();
    test_percentiles();
    printf("perf_hist: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
