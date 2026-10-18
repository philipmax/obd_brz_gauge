/* Verify the real CRC wrapper, complete wire bytes and decoder decisions.
 * Portable codec helpers are shared; only CRC has a frozen independent oracle. */
#include <stdio.h>
#include <string.h>
#include "espnow_v6.h"
#ifdef EH_HAVE_ROM
#include "esp_rom_crc.h"
#endif

uint16_t reference_crc16(const uint8_t *p, size_t n);
uint16_t portable_espnow_crc16(const uint8_t *p, size_t n);
size_t portable_espnow_v6_encode(const espnow_snapshot_t *s, int frag, uint8_t *out, size_t cap);
bool portable_espnow_v6_decode(const uint8_t *buf, size_t len, espnow_rx_frame_t *f);

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { if (failures++ < 10) \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t rng = 0x749AE6D1u;
static uint32_t random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}

static void test_values(void)
{
    uint8_t buf[4104];
    CHECK(espnow_crc16(NULL, 0) == 0xFFFFu);
    CHECK(espnow_crc16((const uint8_t *)"123456789", 9) == 0x29B1u);
    for (unsigned word = 0; word < 65536; word++) {
        unsigned offset = word % 8;
        buf[offset] = (uint8_t)(word >> 8); buf[offset + 1] = (uint8_t)word;
        CHECK(espnow_crc16(buf + offset, 2) == reference_crc16(buf + offset, 2));
    }
    for (unsigned pattern = 0; pattern < 4; pattern++) {
        for (size_t i = 0; i < sizeof(buf); i++)
            buf[i] = pattern == 0 ? 0 : pattern == 1 ? 255 :
                     pattern == 2 ? (uint8_t)i : (uint8_t)random32();
        for (unsigned offset = 0; offset < 8; offset++)
            for (size_t n = 0; n <= 512; n++) {
                uint16_t ref = reference_crc16(buf + offset, n);
                CHECK(espnow_crc16(buf + offset, n) == ref);
                CHECK(portable_espnow_crc16(buf + offset, n) == ref);
            }
    }
    for (unsigned i = 0; i < 200; i++) {
        size_t offset = random32() % 8u, n = random32() % 4097u;
        for (size_t j = 0; j < n; j++) buf[offset + j] = (uint8_t)random32();
        CHECK(espnow_crc16(buf + offset, n) == reference_crc16(buf + offset, n));
    }
}

#ifdef EH_HAVE_ROM
static void test_transitions(void)
{
    /* Every state/input transition of the official backend must match the old
     * bit recurrence. This checks continued calls, not just the fixed seed. */
    for (unsigned state = 0; state < 65536; state++) {
        for (unsigned byte = 0; byte < 256; byte++) {
            uint8_t b = (uint8_t)byte;
            uint16_t ref = (uint16_t)(state ^ (byte << 8));
            for (unsigned bit = 0; bit < 8; bit++)
                ref = ref & 0x8000u ? (uint16_t)((ref << 1) ^ 0x1021u) : (uint16_t)(ref << 1);
            CHECK((uint16_t)~esp_rom_crc16_be((uint16_t)~state, &b, 1) == ref);
        }
    }
    uint8_t data[257];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)random32();
    for (size_t split = 0; split <= sizeof(data); split++) {
        uint16_t state = esp_rom_crc16_be(0, data, (uint32_t)split);
        state = esp_rom_crc16_be(state, data + split, (uint32_t)(sizeof(data) - split));
        CHECK((uint16_t)~state == reference_crc16(data, sizeof(data)));
    }
}
#endif

static void same_decode(const uint8_t *a, const uint8_t *b, size_t n, bool expected)
{
    espnow_rx_frame_t fa, fb;
    bool oka = espnow_v6_decode(a, n, &fa);
    bool okb = portable_espnow_v6_decode(b, n, &fb);
    CHECK(oka == expected); CHECK(oka == okb);
    if (oka && okb) {
        CHECK(!memcmp(fa.v6_rec, fb.v6_rec, (size_t)fa.n_rec * ESPNOW_V6_REC_LEN));
        fa.v6_rec = fb.v6_rec = NULL;
        CHECK(!memcmp(&fa, &fb, sizeof(fa)));
    }
}

static void test_frames(void)
{
    uint8_t a[ESPNOW_FRAME_MAX + 8], b[ESPNOW_FRAME_MAX + 8];
    espnow_snapshot_t s;
    for (unsigned records = 0; records <= ESPNOW_SNAP_MAX_REC; records++) {
        for (unsigned offset = 0; offset < 8; offset++) {
            espnow_snapshot_init(&s);
            s.seq = random32(); s.profile_hash = random32();
            s.flags = (uint8_t)random32(); s.sweep_step = (uint8_t)random32();
            s.intro_step = (uint8_t)random32(); s.gear = (int8_t)(random32() % 8u);
            s.std_valid = random32(); s.veh_valid = random32();
            for (size_t i = 0; i < sizeof(s.name) - 1; i++) s.name[i] = (char)('A' + random32() % 26u);
            s.name[sizeof(s.name) - 1] = '\0';
            /* All 64 vehicle records are legal wire IDs, including ones beyond
             * the 32-bit validity mask; payload values cover every sign bit. */
            for (unsigned i = 0; i < records; i++) {
                CHECK(espnow_snapshot_put(&s, (uint16_t)(ESPNOW_ID_VEH_FIRST + i),
                                         (int32_t)random32()));
            }
            for (int frag = 0; frag < espnow_v6_frame_count(&s); frag++) {
                memset(a, 0xA5, sizeof(a)); memset(b, 0xA5, sizeof(b));
                size_t n = espnow_v6_encode(&s, frag, a + offset, ESPNOW_FRAME_MAX);
                size_t old_n = portable_espnow_v6_encode(&s, frag, b + offset, ESPNOW_FRAME_MAX);
                CHECK(n == old_n); CHECK(n > 0); CHECK(!memcmp(a, b, sizeof(a)));
                if (!n) continue;
                uint16_t crc = reference_crc16(a + offset, n - 2);
                CHECK(a[offset + n - 2] == (uint8_t)crc);
                CHECK(a[offset + n - 1] == (uint8_t)(crc >> 8));
                same_decode(a + offset, b + offset, n, true);
                /* Truncation decisions and every byte position, including CRC. */
                for (size_t cut = 0; cut < n; cut++)
                    same_decode(a + offset, b + offset, cut, false);
                for (size_t pos = 0; pos < n; pos++) {
                    uint8_t flip = (uint8_t)(1u << (pos % 8));
                    a[offset + pos] ^= flip; b[offset + pos] ^= flip;
                    same_decode(a + offset, b + offset, n, false);
                    a[offset + pos] ^= flip; b[offset + pos] ^= flip;
                }
                CHECK(espnow_v6_encode(&s, frag, a + offset, n - 1) == 0);
                CHECK(portable_espnow_v6_encode(&s, frag, b + offset, n - 1) == 0);
            }
        }
    }
}

int main(void)
{
    test_values();
#ifdef EH_HAVE_ROM
    test_transitions();
#endif
    test_frames();
    printf("espnow_crc: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
