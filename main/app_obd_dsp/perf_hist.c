#include "perf_hist.h"

#include <string.h>

void perf_hist_reset(perf_hist_t *hist)
{
    memset(hist, 0, sizeof(*hist));
    hist->min_us = UINT32_MAX;
}

bool perf_hist_add(perf_hist_t *hist, uint32_t duration_us, uint32_t units)
{
    if (hist->samples == UINT32_MAX) return false;
    unsigned bucket = duration_us <= 1 ? 0 : 32u - (unsigned)__builtin_clz(duration_us - 1u);
    if (bucket >= PERF_HIST_BUCKETS) bucket = PERF_HIST_BUCKETS - 1;
    hist->buckets[bucket]++;
    hist->samples++;
    if (duration_us < hist->min_us) hist->min_us = duration_us;
    if (duration_us > hist->max_us) hist->max_us = duration_us;
    hist->total_us += duration_us;
    hist->units += units;
    return true;
}

uint32_t perf_hist_percentile_upper(const perf_hist_t *hist, unsigned percentile)
{
    if (!hist->samples || !percentile || percentile > 100) return 0;
    uint32_t rank = (uint32_t)(((uint64_t)hist->samples * percentile + 99u) / 100u);
    uint64_t cumulative = 0;
    for (unsigned i = 0; i < PERF_HIST_BUCKETS; i++) {
        cumulative += hist->buckets[i];
        if (cumulative >= rank) return i == PERF_HIST_BUCKETS - 1 ? UINT32_MAX : 1u << i;
    }
    return 0;
}
