#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "espnow_v6.h"

bool snapshot_put_reference(espnow_snapshot_t *s, uint16_t id, int32_t value);
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { if (failures++ < 10) \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t rng = 0x52CF1831;
static uint32_t random32(void)
{ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void compare_insert(espnow_snapshot_t *a, espnow_snapshot_t *b, uint16_t id, int32_t value, bool unique)
{
    CHECK((unique ? espnow_snapshot_append_unique(a, id, value) : espnow_snapshot_put(a, id, value)) ==
        snapshot_put_reference(b, id, value));
    CHECK(!memcmp(a, b, sizeof(*a)));
}
static void test_all_ids(void)
{
    for (unsigned id = 0; id < 65536; id++) {
        espnow_snapshot_t a, b;
        espnow_snapshot_init(&a); espnow_snapshot_init(&b);
        compare_insert(&a, &b, (uint16_t)id, (int32_t)(id * 37) - 1000, true);
        /* Invalid/mixed metadata is also preserved rather than trusted as a lookup index. */
        a.std_valid = b.std_valid = 0xA571930Fu;
        a.veh_valid = b.veh_valid = 0x731BC19Au;
        compare_insert(&a, &b, (uint16_t)id, -(int32_t)id, false);
        espnow_snapshot_init(&a); espnow_snapshot_init(&b);
        a.std_valid = b.std_valid = 0xA571930Fu;
        a.veh_valid = b.veh_valid = 0x731BC19Au;
        compare_insert(&a, &b, (uint16_t)id, (int32_t)id, true);
        espnow_snapshot_init(&a); espnow_snapshot_init(&b);
        for (int i = 0; i < ESPNOW_SNAP_MAX_REC; i++) {
            a.rec[i].num_id = b.rec[i].num_id = (uint16_t)(0x4000 + i);
            a.rec[i].value = b.rec[i].value = -i;
        }
        a.n_rec = b.n_rec = ESPNOW_SNAP_MAX_REC;
        if (id < 0x4000 || id >= 0x4000 + ESPNOW_SNAP_MAX_REC)
            compare_insert(&a, &b, (uint16_t)id, INT32_MAX, true);
        compare_insert(&a, &b, (uint16_t)id, INT32_MIN, false);
    }
    const int32_t values[] = { INT32_MIN, -129, -128, -1, 0, 8, 126, 127, 128, INT32_MAX };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        espnow_snapshot_t a, b;
        espnow_snapshot_init(&a); espnow_snapshot_init(&b);
        compare_insert(&a, &b, ESPNOW_ID_GEAR, values[i], true);
        compare_insert(&a, &b, ESPNOW_ID_GEAR, values[i], true);
        CHECK(!espnow_snapshot_append_unique(NULL, ESPNOW_ID_GEAR, values[i]));
        CHECK(!espnow_snapshot_append_unique(NULL, ESPNOW_ID_RPM, values[i]));
    }
}
static void test_sequences(void)
{
    for (unsigned run = 0; run < 4096; run++) {
        espnow_snapshot_t a, b;
        uint16_t ids[64];
        espnow_snapshot_init(&a); espnow_snapshot_init(&b);
        for (unsigned i = 0; i < 64; i++)
            ids[i] = i < 12 ? (uint16_t)(1 + i) : i < 44 ? (uint16_t)(0x100 + i - 12) : (uint16_t)(0x400 + i - 44);
        for (unsigned i = 63; i; i--) {
            unsigned j = random32() % (i + 1); uint16_t t = ids[i]; ids[i] = ids[j]; ids[j] = t;
        }
        unsigned count = run % 65;
        for (unsigned i = 0; i < count; i++) {
            int32_t value = ids[i] == ESPNOW_ID_GEAR ? (int32_t)(run % 10) - 1 : (int32_t)(random32() & INT32_MAX);
            compare_insert(&a, &b, ids[i], value, true);
        }
        /* Linked RPM override deliberately replaces or appends through the general API. */
        compare_insert(&a, &b, ESPNOW_ID_RPM, (int32_t)run, false);
        for (unsigned i = 0; i < count; i++) compare_insert(&a, &b, ids[i], -(int32_t)i, false);
    }
    /* Capacity without gear: exactly 64 unique records, then one rejected append. */
    espnow_snapshot_t a, b;
    espnow_snapshot_init(&a); espnow_snapshot_init(&b);
    for (int i = 0; i < 65; i++) compare_insert(&a, &b, (uint16_t)(0x100 + i), i, true);
    compare_insert(&a, &b, ESPNOW_ID_GEAR, 3, true);
    compare_insert(&a, &b, 0x100, 42, false); /* Replacement still works at capacity. */
}
int main(void)
{
    test_all_ids(); test_sequences();
    printf("snapshot_append: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
