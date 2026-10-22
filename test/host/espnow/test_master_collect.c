#include <stdio.h>
#include <string.h>
#include "collect_probe.h"

static unsigned checks, failures, cases;
#define CHECK(c) do { checks++; if (!(c)) { if (failures++ < 10) \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void one(collect_input_t *input)
{
    espnow_snapshot_t a, b;
    memset(&a, 0xB5, sizeof(a)); memset(&b, 0x71, sizeof(b));
    collect_probe_current(input, &a); collect_probe_reference(input, &b);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    CHECK(a.n_rec <= SIG_OBD_COUNT - 1 + SIG_VEH_MAX);
    uint8_t old[ESPNOW_FRAME_MAX], next[ESPNOW_FRAME_MAX];
    size_t n = espnow_v5_encode(&a, next, sizeof(next));
    CHECK(n == espnow_v5_encode(&b, old, sizeof(old)) && n && !memcmp(old, next, n));
    int frames = espnow_v6_frame_count(&a);
    CHECK(frames == espnow_v6_frame_count(&b));
    for (int i = 0; i < frames; i++) {
        n = espnow_v6_encode(&a, i, next, sizeof(next));
        CHECK(n == espnow_v6_encode(&b, i, old, sizeof(old)) && n && !memcmp(old, next, n));
        espnow_rx_frame_t rx;
        CHECK(espnow_v6_decode(next, n, &rx));
    }
    cases++;
}
int main(void)
{
    collect_input_t in;
    memset(&in, 0, sizeof(in));
    for (int i = 0; i < SIG_SLOT_COUNT; i++) in.values[i] = i * 317 - 100;
    in.seq_before = UINT32_MAX; in.hash = 0x18235AD7; in.sweep = 513; in.intro = -1;
    const int counts[] = { 0, 1, 16, 32 };
    for (unsigned mask = 0; mask < (1u << SIG_OBD_COUNT); mask++) {
        for (unsigned n = 0; n < sizeof(counts) / sizeof(counts[0]); n++) {
            in.reported_vehicle_count = in.present_vehicle_count = counts[n];
            uint64_t dyn = UINT64_C(0xB371C57E) << SIG_VEH_BASE;
            in.valid_slots = mask | dyn | (UINT64_C(1) << SIG_SYS_OBD_LINK);
            in.values[SIG_SYS_OBD_LINK] = (int)(mask % 4); in.values[SIG_OBD_GEAR] = (int)(mask % 260) - 129;
            in.cfg.rpm_warn_linked_en = (mask % 2) != 0;
            for (int active = 0; active <= 1; active++) {
                in.test_active = active != 0; in.test_rpm = (uint16_t)(mask * 37);
                one(&in);
            }
        }
    }
    in.values[SIG_OBD_GEAR] = 4; in.test_active = true; in.test_rpm = UINT16_MAX;
    for (int n = 0; n <= SIG_VEH_MAX + 1; n++) {
        in.reported_vehicle_count = n; in.present_vehicle_count = n > SIG_VEH_MAX ? SIG_VEH_MAX : n;
        for (int hole = -1; hole < SIG_VEH_MAX; hole++) {
            in.valid_slots = (UINT64_C(1) << SIG_SLOT_COUNT) - 1;
            if (hole >= 0) in.valid_slots &= ~(UINT64_C(1) << (SIG_VEH_BASE + hole));
            one(&in);
            in.present_vehicle_count = n / 2; one(&in); /* Slots disappear between the batches. */
            in.present_vehicle_count = n > SIG_VEH_MAX ? SIG_VEH_MAX : n;
        }
    }
    printf("master_collect: %u cases, %u checks, %u failures\n", cases, checks, failures);
    return failures ? 1 : 0;
}
