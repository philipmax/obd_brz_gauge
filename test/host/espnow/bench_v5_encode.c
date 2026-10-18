/* Host-only: pure encoder or actual collector with deterministic store/UI stubs.
 * No real lock, RF, RTOS, PSRAM/cache or device timing is represented. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "collect_probe.h"
size_t v5_encode_reference(const espnow_snapshot_t *, uint8_t *, size_t);
static volatile uint32_t sink;
static espnow_snapshot_t snapshot;
static collect_input_t input;
static bool with_collect;
static uint64_t now_ns(void)
{ struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) abort(); return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec; }
static double sample(bool old)
{
    size_t (*encode)(const espnow_snapshot_t *, uint8_t *, size_t) = old ? v5_encode_reference : espnow_v5_encode;
    uint64_t start = now_ns(), elapsed, count = 0;
    uint32_t sum = 0;
    do {
        for (unsigned i = 0; i < 128; i++) {
            espnow_snapshot_t s;
            const espnow_snapshot_t *p = &snapshot;
            if (with_collect) { collect_probe_current(&input, &s); p = &s; }
            uint8_t out[ESPNOW_V5_DATA_LEN];
            size_t n = encode(p, out, sizeof(out));
            sum += (uint32_t)n + out[i % ESPNOW_V5_DATA_LEN];
        }
        count += 128; elapsed = now_ns() - start;
    } while (elapsed < 20000000);
    sink ^= sum; return (double)elapsed / (double)count;
}
static int cmp(const void *a, const void *b)
{ double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static void row(const char *name)
{
    if (with_collect) collect_probe_current(&input, &snapshot);
    uint8_t a[47], b[47];
    if (v5_encode_reference(&snapshot, a, sizeof(a)) != espnow_v5_encode(&snapshot, b, sizeof(b)) || memcmp(a, b, sizeof(a))) abort();
    double old[7], next[7], ratio[7], raw_old[7], raw_next[7];
    for (unsigned group = 0; group < 7; group++) {
        if (group % 2) { next[group] = sample(false); old[group] = sample(true); }
        else { old[group] = sample(true); next[group] = sample(false); }
        ratio[group] = next[group] / old[group];
    }
    memcpy(raw_old, old, sizeof(old)); memcpy(raw_next, next, sizeof(next));
    qsort(old, 7, sizeof(double), cmp); qsort(next, 7, sizeof(double), cmp); qsort(ratio, 7, sizeof(double), cmp);
    printf("%s,%u,%u,%.3f,%.3f,%.6f,%.6f,%.6f", name, with_collect, snapshot.n_rec, old[3], next[3], ratio[3], ratio[0], ratio[6]);
    for (unsigned i = 0; i < 7; i++) printf(",%.3f", raw_old[i]);
    for (unsigned i = 0; i < 7; i++) printf(",%.3f", raw_next[i]);
    putchar('\n');
}
static void add(uint16_t id, int32_t value)
{ if (!espnow_snapshot_put(&snapshot, id, value)) abort(); }
static void standard(bool reverse)
{ for (int i = 0; i < 12; i++) { int id = reverse ? 12 - i : 1 + i; add((uint16_t)id, id == 6 ? 3 : id * 37); } }
static void dynamic(int count)
{ for (int i = 0; i < count; i++) add((uint16_t)(256 + i), -i * 37); }
int main(void)
{
    puts("case,with_collect,records,old_ns,new_ns,new_over_old,ratio_min,ratio_max,old_0_ns,old_1_ns,old_2_ns,old_3_ns,old_4_ns,old_5_ns,old_6_ns,new_0_ns,new_1_ns,new_2_ns,new_3_ns,new_4_ns,new_5_ns,new_6_ns");
    const char *names[] = {"empty", "rpm", "standard", "full_standard_first", "full_standard_last", "dynamic32", "unknown64", "reverse_standard", "duplicates64", "partial_full", "mixed_mask"};
    for (unsigned j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
        espnow_snapshot_init(&snapshot);
        if (j == 1) add(1, 1234);
        if (j == 4 || j == 5) dynamic(32);
        if (j == 2 || j == 3 || j == 4 || j == 10) standard(false);
        if (j == 3) dynamic(32);
        if (j == 6) for (int id = 512; id < 576; id++) add((uint16_t)id, id);
        if (j == 7) standard(true);
        if (j == 8) { snapshot.n_rec = 64; for (int i = 0; i < 64; i++) { snapshot.rec[i].num_id = (uint16_t)(1 + i % 12); snapshot.rec[i].value = i * 37; } }
        if (j == 9) { add(1, 1234); add(4, 90); dynamic(32); }
        if (j == 10) snapshot.std_valid = 0;
        row(names[j]);
    }
    with_collect = true;
    const char *collect_names[] = {"collect_empty", "collect_standard", "collect_dynamic1", "collect_full", "collect_sparse", "collect_override"};
    for (unsigned j = 0; j < sizeof(collect_names) / sizeof(collect_names[0]); j++) {
        memset(&input, 0, sizeof(input)); input.hash = 0x12345678; input.seq_before = 44;
        input.reported_vehicle_count = input.present_vehicle_count = j == 2 ? 1 : j >= 3 ? 32 : 0;
        input.sweep = 22; input.intro = 17;
        for (int slot = 0; slot < SIG_SLOT_COUNT; slot++) {
            input.values[slot] = slot == SIG_OBD_GEAR ? 3 : slot * 37;
            if (j && (j != 4 || slot % 3)) input.valid_slots |= UINT64_C(1) << slot;
        }
        input.test_active = j == 5; input.test_rpm = 6553;
        row(collect_names[j]);
    }
    return 0;
}
