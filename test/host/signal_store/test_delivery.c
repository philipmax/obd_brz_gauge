#include "delivery_reference.h"
#include "app_obd_dsp/signal_store.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
int64_t esp_timer_get_time(void) { return 0; }
void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(n, size);
}
static void check(int condition, const char *what, int slot, int32_t raw)
{
    checks++;
    if (!condition) {
        if (failures < 10) fprintf(stderr, "%s: slot=%d raw=%d\n", what, slot, raw);
        failures++;
    }
}
static void compare(int slot, int32_t raw)
{
    double a = delivery_reference(slot, raw), b = delivery_candidate(slot, raw);
    uint64_t ab, bb;
    memcpy(&ab, &a, sizeof(ab)); memcpy(&bb, &b, sizeof(bb));
    check(ab == bb, "different delivered-value bits", slot, raw);
}
static uint32_t next(uint32_t *seed)
{
    uint32_t x = *seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *seed = x;
}
int main(void)
{
    static const int32_t corners[] = {INT32_MIN, -32001, -2001, -2000, -101, -100,
        -99, -1, 0, 1, 99, 100, 101, 1999, 2000, 2001, 31999, 32000, 32001, INT32_MAX};
    for (int slot = 0; slot < SIG_SLOT_COUNT; slot++) {
        for (unsigned i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) compare(slot, corners[i]);
    }
    /* All possible valid raw values of both accelerated signals. */
    for (int32_t raw = 0; raw <= 2000; raw++) compare(SIG_OBD_OIL_PRESSURE, raw);
    for (int32_t raw = 0; raw <= 32000; raw++) compare(SIG_OBD_BATTERY_VOLTAGE, raw);
    /* Negative battery values are not valid samples; still preserve floor. */
    for (int32_t raw = -32000; raw < 0; raw++) compare(SIG_OBD_BATTERY_VOLTAGE, raw);
    uint32_t seed = 0x39049bd1u;
    for (unsigned step = 0; step < 200000; step++) {
        uint32_t bits = next(&seed);
        int32_t raw;
        memcpy(&raw, &bits, sizeof(raw));
        int slot = (int)(next(&seed) % SIG_SLOT_COUNT);
        compare(slot, raw);
        compare(SIG_OBD_BATTERY_VOLTAGE, raw);
    }
    /* Exercise the public snapshot path, including validity and link gating. */
    signal_store_init();
    check(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_DATA), "link set", SIG_SYS_OBD_LINK, SIG_LINK_DATA);
    const int slots[] = {SIG_OBD_OIL_PRESSURE, SIG_OBD_BATTERY_VOLTAGE};
    for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        int slot = slots[i];
        uint64_t bit = UINT64_C(1) << slot;
        double values[SIG_SLOT_COUNT];
        for (int32_t raw = 0; raw <= sig_registry[slot].max; raw++) {
            check(signal_store_set(slot, raw), "valid set rejected", slot, raw);
            uint64_t mask = signal_store_get_delivered_mask(bit, values);
            check(mask == bit, "valid sample missing", slot, raw);
            double expected = delivery_reference(slot, raw);
            check(memcmp(&values[slot], &expected, sizeof(expected)) == 0, "snapshot differs", slot, raw);
        }
        check(!signal_store_set(slot, -1), "invalid set accepted", slot, -1);
        check(signal_store_get_delivered_mask(bit, values) == 0 && isnan(values[slot]), "invalid not NaN", slot, -1);
        check(signal_store_set(slot, 1), "valid restore rejected", slot, 1);
        check(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_NONE), "link down failed", SIG_SYS_OBD_LINK, SIG_LINK_NONE);
        check(signal_store_get_delivered_mask(bit, values) == 0 && isnan(values[slot]), "link gating changed", slot, 1);
        check(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_DATA), "link restore failed", SIG_SYS_OBD_LINK, SIG_LINK_DATA);
    }
    printf("signal_delivery: checks=%u fail=%u\n", checks, failures);
    return failures ? 1 : 0;
}
