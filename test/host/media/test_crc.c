/* Same vectors exercise the portable and ESP_PLATFORM wrappers. The ROM test
 * links ESP-IDF's official Linux implementation, not a project-owned mock.
 * Fixed expected values below were generated independently with Python zlib.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "obv1_format.h"
#include "vp_common.h"
#include "crc_reference.h"

static unsigned checks;
static unsigned failures;
static uint32_t rng = UINT32_C(0x718a62e9);

static uint32_t random_u32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void equal(uint32_t got, uint32_t expected, unsigned line)
{
    checks++;
    if (got == expected) return;
    if (failures++ < 8) fprintf(stderr, "line %u: %08" PRIx32 " != %08" PRIx32 "\n",
                                line, got, expected);
}
#define EQ(got, want) equal((got), (want), __LINE__)

static void check_buffer(uint32_t seed, const uint8_t *buf, size_t len)
{
    const uint32_t expected = crc_bitwise_reference(seed, buf, len);
    EQ(crc_nibble_reference(seed, buf, len), expected);
    EQ(vp_crc32(seed, buf, len), expected);
    EQ(obv1_crc32(seed, buf, len), expected);

    /* Arbitrary chunks + empty updates preserve the incremental convention. */
    uint32_t vehicle = seed, video = seed;
    size_t off = 0;
    while (off < len) {
        size_t chunk = (size_t)(random_u32() % 193u) + 1;
        if (chunk > len - off) chunk = len - off;
        EQ(vp_crc32(vehicle, NULL, 0), vehicle);
        EQ(obv1_crc32(video, NULL, 0), video);
        vehicle = vp_crc32(vehicle, buf + off, chunk);
        video = obv1_crc32(video, buf + off, chunk);
        off += chunk;
    }
    EQ(vehicle, expected);
    EQ(video, expected);
    if (len) {
        /* Corruption must be detected by both paths. */
        uint32_t v = vp_crc32(seed, buf, len - 1);
        const uint8_t last = (uint8_t)(buf[len - 1] ^ 1u);
        v = vp_crc32(v, &last, 1);
        EQ(v == expected, 0);
        uint32_t o = obv1_crc32(seed, buf, len - 1);
        o = obv1_crc32(o, &last, 1);
        EQ(o, v);
    }
}

int main(void)
{
    static const uint32_t seeds[] = {0, 1, UINT32_MAX, UINT32_C(0xdeadbeef)};
    static const size_t boundaries[] = {255, 256, 511, 512, 1023, 1024, 4095, 4096,
                                      8191, 8192, 16383, 16384, 32767, 32768, 288000};
    static const struct {
        uint32_t seed;
        size_t off, len;
        uint32_t expected;
    } vectors[] = {
        {0x00000000u, 0, 0, 0x00000000u},
        {0x00000000u, 0, 60, 0xdeff87afu},
        {0x00000000u, 3, 1024, 0x0039d067u},
        {0xdeadbeefu, 1, 8192, 0x7d005326u},
        {0xffffffffu, 7, 32768, 0x29425881u},
        {0x12345678u, 2, 288000, 0x90516492u},
    };
    uint8_t *buf = malloc(288008);
    if (!buf) return 2;
    for (size_t i = 0; i < 288008; i++) buf[i] = (uint8_t)(i * 37u + i / 7u + 19u);
    EQ(vp_crc32(0, "123456789", 9), UINT32_C(0xcbf43926));
    EQ(obv1_crc32(0, "123456789", 9), UINT32_C(0xcbf43926));
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        EQ(vp_crc32(vectors[i].seed, buf + vectors[i].off, vectors[i].len), vectors[i].expected);
        EQ(obv1_crc32(vectors[i].seed, buf + vectors[i].off, vectors[i].len), vectors[i].expected);
    }
    for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
        EQ(vp_crc32(seeds[s], NULL, 0), seeds[s]);
        EQ(obv1_crc32(seeds[s], NULL, 0), seeds[s]);
        for (size_t off = 0; off < 8; off++) {
            for (size_t len = 0; len <= 128; len++) check_buffer(seeds[s], buf + off, len);
            for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
                check_buffer(seeds[s], buf + off, boundaries[i]);
            }
        }
    }
    for (unsigned i = 0; i < 2048; i++) {
        uint32_t seed = random_u32();
        size_t off = random_u32() & 7u;
        size_t len = random_u32() & 4095u;
        check_buffer(seed, buf + off, len);
    }
    free(buf);
    printf("CRC checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
