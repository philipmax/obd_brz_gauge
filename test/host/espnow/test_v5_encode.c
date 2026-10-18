/* Wire equivalence, including malformed-but-bounded record lists and guards. */
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "espnow_v6.h"
size_t v5_encode_reference(const espnow_snapshot_t *, uint8_t *, size_t);
static unsigned checks, failures, cases;
#define CHECK(c) do { checks++; if (!(c)) { if (failures++ < 10) fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t rng = 0x146AC759;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void compare(const espnow_snapshot_t *s, size_t cap)
{
    uint8_t a[ESPNOW_V5_DATA_LEN + 16], b[sizeof(a)];
    espnow_snapshot_t before = *s;
    memset(a, 0xA5, sizeof(a)); memset(b, 0xA5, sizeof(b));
    size_t n = v5_encode_reference(s, a + 8, cap), m = espnow_v5_encode(s, b + 8, cap);
    CHECK(n == m); CHECK(!memcmp(a, b, sizeof(a))); CHECK(!memcmp(s, &before, sizeof(*s)));
    if (m) { CHECK(b[8 + ESPNOW_V5_RETIRED_OFS] == (uint8_t)ESPNOW_V5_RETIRED_VALUE);
        CHECK(b[8 + ESPNOW_V5_RETIRED_OFS + 1] == (uint8_t)((uint16_t)ESPNOW_V5_RETIRED_VALUE >> 8)); }
    cases++;
}
static int32_t safe_value(uint16_t id, uint32_t bits)
{
    int32_t v; memcpy(&v, &bits, sizeof(v));
    /* Existing div10_half_up arithmetic is defined over the registry's bounded
     * pressure/boost domain. Exclude its pre-existing extreme-int overflow. */
    if (id == ESPNOW_ID_BOOST || id == ESPNOW_ID_OIL_PRESSURE) v = v % 10000000;
    return v;
}
int main(void)
{
    espnow_snapshot_t s; espnow_snapshot_init(&s);
    uint8_t bytes[47]; memset(bytes, 0xA5, sizeof(bytes));
    CHECK(!espnow_v5_encode(NULL, bytes, sizeof(bytes))); CHECK(bytes[0] == 0xA5);
    CHECK(!espnow_v5_encode(&s, NULL, sizeof(bytes)));
    for (size_t cap = 0; cap <= 55; cap++) compare(&s, cap);
    for (unsigned id = 0; id <= UINT16_MAX; id++) {
        espnow_snapshot_init(&s); s.n_rec = 3;
        s.rec[0].num_id = (uint16_t)id; s.rec[0].value = safe_value((uint16_t)id, random32());
        s.rec[1] = s.rec[0]; s.rec[1].value = 123;
        s.rec[2].num_id = ESPNOW_ID_GEAR; s.rec[2].value = 7;
        s.std_valid = random32(); s.veh_valid = random32(); compare(&s, 47);
    }
    static const int32_t values[] = { INT32_MIN, INT32_MIN + 5, -327681, -327680, -327675,
        -32768, -1001, -1000, -101, -100, -41, -40, -21, -20, -16, -15, -14, -6, -5, -4,
        -1, 0, 1, 4, 5, 6, 14, 15, 16, 150, 255, 256, 32767, 32768, 65535, 65536,
        327674, 327675, 327680, INT32_MAX - 5, INT32_MAX };
    for (unsigned id = 1; id <= 12; id++) for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
        if ((id == ESPNOW_ID_BOOST || id == ESPNOW_ID_OIL_PRESSURE) &&
            (values[j] < INT32_MIN + 6 || values[j] > INT32_MAX - 5)) continue;
        espnow_snapshot_init(&s); s.n_rec = 2; s.rec[0].num_id = (uint16_t)id;
        s.rec[0].value = values[j]; s.rec[1] = s.rec[0]; s.rec[1].value = 4321; compare(&s, 47);
    }
    for (unsigned mask = 0; mask < 4096; mask++) {
        espnow_snapshot_init(&s);
        for (unsigned id = 1; id <= 12; id++) if (mask & (1u << (id - 1))) {
            s.rec[s.n_rec].num_id = (uint16_t)id; s.rec[s.n_rec++].value = (int32_t)(id * 37) - 100;
        }
        s.std_valid = ~mask; s.veh_valid = mask;
        for (int g = -128; g <= 127; g++) { s.gear = (int8_t)g; compare(&s, 47); }
    }
    for (unsigned run = 0; run < 32768; run++) {
        memset(&s, 0, sizeof(s)); s.n_rec = (uint8_t)(run % 65);
        s.flags = (uint8_t)random32(); s.seq = random32(); s.std_valid = random32();
        s.veh_valid = random32(); s.gear = (int8_t)random32(); s.sweep_step = (uint8_t)random32();
        s.intro_step = (uint8_t)random32();
        for (unsigned i = 0; i < sizeof(s.name); i++) s.name[i] = (char)random32();
        for (unsigned i = 0; i < s.n_rec; i++) {
            uint16_t id = run % 3 == 0 ? (uint16_t)(1 + random32() % 12) :
                run % 3 == 1 ? (uint16_t)(random32() % 512) : (uint16_t)random32();
            s.rec[i].num_id = id; s.rec[i].value = safe_value(id, random32());
        }
        compare(&s, 47);
    }
    printf("v5_encode: %u cases, %u checks, %u failures\n", cases, checks, failures);
    return failures ? 1 : 0;
}
