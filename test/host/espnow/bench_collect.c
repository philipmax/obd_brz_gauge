/* Host timing of the real collector with deterministic store/UI stubs.
 * Excludes FreeRTOS locks, RF, production store reads and device CRC timing. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "collect_probe.h"

bool snapshot_put_reference(espnow_snapshot_t *s, uint16_t id, int32_t value);
static volatile uint32_t sink;
static collect_input_t input;
static unsigned n_ids;
static uint16_t ids[ESPNOW_SNAP_MAX_REC];
static bool override_rpm;

typedef enum { INSERT, COLLECT, COLLECT_V5, COLLECT_V6 } bench_mode_t;
static void insertion(bool old, espnow_snapshot_t *out)
{
    bool (*put)(espnow_snapshot_t *, uint16_t, int32_t) =
        old ? snapshot_put_reference : espnow_snapshot_append_unique;
    espnow_snapshot_init(out);
    for (unsigned i = 0; i < n_ids; i++)
        if (!put(out, ids[i], (int32_t)i * 79 - 20)) abort();
    if (!put(out, ESPNOW_ID_GEAR, 4)) abort();
    if (override_rpm && !espnow_snapshot_put(out, ESPNOW_ID_RPM, 6543)) abort();
}
static void collect(bool old, espnow_snapshot_t *out)
{
    if (old) collect_probe_reference(&input, out);
    else collect_probe_current(&input, out);
}
static uint32_t work(bench_mode_t mode, bool old)
{
    espnow_snapshot_t s;
    uint8_t bytes[ESPNOW_FRAME_MAX];
    if (mode == INSERT) insertion(old, &s);
    else collect(old, &s);
    uint32_t sum = s.std_valid ^ s.veh_valid ^ s.seq ^ (uint32_t)s.gear;
    for (unsigned i = 0; i < s.n_rec; i++) sum += (uint32_t)s.rec[i].value + s.rec[i].num_id;
    if (mode == COLLECT_V5) {
        size_t n = espnow_v5_encode(&s, bytes, sizeof(bytes));
        if (!n) abort();
        sum += bytes[n - 1] + bytes[10] + (uint32_t)n;
    } else if (mode == COLLECT_V6) {
        int frames = espnow_v6_frame_count(&s);
        for (int i = 0; i < frames; i++) {
            size_t n = espnow_v6_encode(&s, i, bytes, sizeof(bytes));
            if (!n) abort();
            sum += bytes[n - 1] + bytes[10] + (uint32_t)n;
        }
    }
    return sum;
}
static double now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static double sample(bench_mode_t mode, bool old)
{
    uint64_t count = 0;
    uint32_t sum = 0;
    double start = now(), elapsed;
    do {
        for (unsigned i = 0; i < 128; i++) sum += work(mode, old);
        count += 128;
        elapsed = now() - start;
    } while (elapsed < 0.020);
    sink ^= sum;
    return elapsed * 1e9 / (double)count;
}
static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void row(const char *name, bench_mode_t mode)
{
    espnow_snapshot_t a, b;
    if (mode == INSERT) { insertion(true, &a); insertion(false, &b); }
    else { collect(true, &a); collect(false, &b); }
    if (memcmp(&a, &b, sizeof(a)) || work(mode, true) != work(mode, false)) abort();
    uint8_t old_bytes[ESPNOW_FRAME_MAX], new_bytes[ESPNOW_FRAME_MAX];
    for (int f = -1; f < espnow_v6_frame_count(&a); f++) {
        size_t n = f < 0 ? espnow_v5_encode(&a, old_bytes, sizeof(old_bytes)) :
            espnow_v6_encode(&a, f, old_bytes, sizeof(old_bytes));
        size_t m = f < 0 ? espnow_v5_encode(&b, new_bytes, sizeof(new_bytes)) :
            espnow_v6_encode(&b, f, new_bytes, sizeof(new_bytes));
        if (!n || n != m || memcmp(old_bytes, new_bytes, n)) abort();
    }
    double old[7], next[7], ratios[7];
    for (unsigned group = 0; group < 7; group++) {
        if (group % 2) { next[group] = sample(mode, false); old[group] = sample(mode, true); }
        else { old[group] = sample(mode, true); next[group] = sample(mode, false); }
        ratios[group] = next[group] / old[group];
    }
    double raw_old[7], raw_next[7];
    memcpy(raw_old, old, sizeof(old)); memcpy(raw_next, next, sizeof(next));
    qsort(ratios, 7, sizeof(double), compare);
    qsort(old, 7, sizeof(double), compare); qsort(next, 7, sizeof(double), compare);
    printf("%s,%u,%d,%.3f,%.3f,%.6f,%.6f,%.6f", name, a.n_rec,
        espnow_v6_frame_count(&a), old[3], next[3], ratios[3], ratios[0], ratios[6]);
    for (unsigned i = 0; i < 7; i++) printf(",%.3f", raw_old[i]);
    for (unsigned i = 0; i < 7; i++) printf(",%.3f", raw_next[i]);
    putchar('\n');
}
int main(void)
{
    puts("case,records,v6_frames,old_ns,new_ns,median_ratio,min_ratio,max_ratio,old1,old2,old3,old4,old5,old6,old7,new1,new2,new3,new4,new5,new6,new7");
    const unsigned counts[] = {0, 1, 11, 32, 43, 64};
    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        n_ids = counts[c];
        for (unsigned i = 0; i < n_ids; i++) ids[i] = i < 11 ?
            (uint16_t)(i + 1 + (i + 1 >= ESPNOW_ID_GEAR)) : (uint16_t)(0x100 + i - 11);
        char name[64];
        override_rpm = false;
        snprintf(name, sizeof(name), "insert_%u", n_ids); row(name, INSERT);
        override_rpm = true;
        snprintf(name, sizeof(name), "insert_%u_override", n_ids); row(name, INSERT);
    }
    for (int c = 0; c < 7; c++) {
        memset(&input, 0, sizeof(input));
        input.hash = 0x7B31287F; input.seq_before = UINT32_MAX;
        input.sweep = 137; input.intro = 88; input.cfg.rpm_warn_linked_en = true;
        for (int i = 0; i < SIG_SLOT_COUNT; i++) input.values[i] = i * 431;
        input.values[SIG_OBD_GEAR] = 4; input.values[SIG_SYS_OBD_LINK] = SIG_LINK_DATA;
        if (c > 0) input.valid_slots = (UINT64_C(1) << SIG_OBD_COUNT) - 1;
        if (c > 1) input.reported_vehicle_count = input.present_vehicle_count = c == 2 ? 1 : 32;
        if (c > 1) input.valid_slots |= UINT64_C(0xFFFFFFFF) << SIG_VEH_BASE;
        input.valid_slots |= UINT64_C(1) << SIG_SYS_OBD_LINK;
        if (c == 4) input.valid_slots &= UINT64_C(0x15555555555555);
        if (c == 5) input.present_vehicle_count = 16;
        input.test_active = c == 6; input.test_rpm = 4567;
        const char *names[] = {"empty", "standard", "dynamic1", "full", "sparse", "removed", "override"};
        const bench_mode_t modes[] = { COLLECT, COLLECT_V5, COLLECT_V6 };
        const char *labels[] = { "collect", "v5", "v6" };
        for (unsigned m = 0; m < 3; m++) {
            char name[64]; snprintf(name, sizeof(name), "%s_%s", labels[m], names[c]); row(name, modes[m]);
        }
    }
    return 0;
}
