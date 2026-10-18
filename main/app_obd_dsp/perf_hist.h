#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Duration buckets: <=1, <=2, ... <=1048576 us, then <=UINT32_MAX.
 * Percentiles report bucket upper bounds, not exact observations. */
#define PERF_HIST_BUCKETS 22

typedef struct {
    uint32_t samples;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t total_us;
    uint64_t units;
    uint32_t buckets[PERF_HIST_BUCKETS];
} perf_hist_t;

void perf_hist_reset(perf_hist_t *hist);
bool perf_hist_add(perf_hist_t *hist, uint32_t duration_us, uint32_t units);
uint32_t perf_hist_percentile_upper(const perf_hist_t *hist, unsigned percentile);
