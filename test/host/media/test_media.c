/* Host tests for main/media/obv1_parser.c and main/media/delta_varint.c.
 * argv[1] (optional) = repo root; enables the bootmedia/slot_a round trip. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "delta_varint.h"
#include "obv1_format.h"

static int s_checks;
static int s_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        s_checks++;                                                            \
        if (!(cond)) {                                                         \
            s_failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        s_checks++;                                                            \
        if (_a != _b) {                                                        \
            s_failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", \
                    __FILE__, __LINE__, #a, _a, #b, _b);                       \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------ OBV1 builder */

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

typedef struct {
    uint8_t buf[4096];
    size_t  len;
} obv_file_t;

#define NFR 3
static const uint32_t k_sizes[NFR] = {40, 25, 60};

static void fix_header_crc(obv_file_t *f) { wr32(f->buf + 0x3C, obv1_crc32(0, f->buf, 0x3C)); }

static void fix_index_crc(obv_file_t *f)
{
    wr32(f->buf + 0x28, obv1_crc32(0, f->buf + OBV1_HEADER_SIZE, NFR * OBV1_INDEX_ENTRY));
    fix_header_crc(f);
}

/* 240x240, 30 fps, 4:2:2, three fake "JPEG" frames (FFD8 .. FFD9). */
static void build_obv(obv_file_t *f)
{
    memset(f, 0, sizeof(*f));
    uint8_t *h = f->buf;
    memcpy(h, "OBV1", 4);
    wr16(h + 0x04, 1);
    wr16(h + 0x06, 64);
    wr16(h + 0x08, 240);
    wr16(h + 0x0A, 240);
    wr16(h + 0x0C, 30);
    h[0x0E] = OBV1_SUB_422;
    h[0x0F] = OBV1_FLAG_BLOCK_SAFE;
    uint32_t data_off = OBV1_HEADER_SIZE + NFR * OBV1_INDEX_ENTRY;
    wr32(h + 0x10, NFR);
    wr32(h + 0x14, OBV1_HEADER_SIZE);
    wr32(h + 0x18, data_off);
    uint32_t off = 0, maxsz = 0;
    for (int i = 0; i < NFR; i++) {
        uint8_t *e = h + OBV1_HEADER_SIZE + i * OBV1_INDEX_ENTRY;
        wr32(e, off);
        wr32(e + 4, k_sizes[i]);
        uint8_t *fr = h + data_off + off;
        memset(fr, 0x11 * (i + 1), k_sizes[i]);
        fr[0] = 0xFF; fr[1] = 0xD8; fr[k_sizes[i] - 2] = 0xFF; fr[k_sizes[i] - 1] = 0xD9;
        off += k_sizes[i];
        if (k_sizes[i] > maxsz) maxsz = k_sizes[i];
    }
    wr32(h + 0x1C, off);
    wr32(h + 0x20, maxsz);
    wr32(h + 0x24, NFR * 1000 / 30);
    wr32(h + 0x2C, obv1_crc32(0, h + data_off, off));
    f->len = data_off + off;
    fix_index_crc(f);
}

static void test_crc(void)
{
    CHECK_EQ(obv1_crc32(0, "123456789", 9), 0xCBF43926u);
    uint32_t c = obv1_crc32(0, "1234", 4);
    CHECK_EQ(obv1_crc32(c, "56789", 5), 0xCBF43926u);
    CHECK_EQ(obv1_crc32(0, "", 0), 0u);
}

static void test_obv_ok(void)
{
    obv_file_t f;
    build_obv(&f);
    obv1_info_t i;
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(i.width, 240);
    CHECK_EQ(i.height, 240);
    CHECK_EQ(i.fps, 30);
    CHECK_EQ(i.subsampling, OBV1_SUB_422);
    CHECK_EQ(i.flags, OBV1_FLAG_BLOCK_SAFE);
    CHECK_EQ(i.frame_count, NFR);
    CHECK_EQ(i.data_offset, 64 + 8 * NFR);
    CHECK_EQ(i.data_size, 125);
    CHECK_EQ(i.max_frame_size, 60);
    CHECK_EQ(obv1_duration_ms(&i), 100);
    CHECK_EQ(obv1_check_index(&i, f.buf + i.index_offset, f.len - i.index_offset), OBV1_OK);
    uint32_t off, size;
    obv1_index_entry(f.buf + i.index_offset, 2, &off, &size);
    CHECK_EQ(off, 65);
    CHECK_EQ(size, 60);
    const uint8_t *fr = f.buf + i.data_offset + off;
    CHECK(fr[0] == 0xFF && fr[1] == 0xD8 && fr[size - 1] == 0xD9);
    CHECK_EQ(obv1_crc32(0, f.buf + i.data_offset, i.data_size), i.data_crc32);
    /* unknown file size skips the data-region check */
    CHECK_EQ(obv1_parse_header(f.buf, 64, 0, 0, &i), OBV1_OK);
}

static void test_obv_truncated(void)
{
    obv_file_t f;
    build_obv(&f);
    obv1_info_t i;
    CHECK_EQ(obv1_parse_header(f.buf, 0, 0, 0, &i), OBV1_ERR_TRUNCATED);
    CHECK_EQ(obv1_parse_header(f.buf, 63, 0, 0, &i), OBV1_ERR_TRUNCATED);
    CHECK_EQ(obv1_parse_header(NULL, 64, 0, 0, &i), OBV1_ERR_TRUNCATED);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, 40, 0, &i), OBV1_ERR_TRUNCATED);
    /* file cut inside the last frame */
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len - 1, 0, &i), OBV1_ERR_TRUNCATED);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_OK);
    /* index shorter than frame_count entries */
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, NFR * 8 - 1), OBV1_ERR_TRUNCATED);
}

static void test_obv_bad_magic_version_crc(void)
{
    obv_file_t f;
    obv1_info_t i;
    build_obv(&f);
    f.buf[3] = '2';
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_MAGIC);

    build_obv(&f);
    memcpy(f.buf, "RIFF", 4);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_MAGIC);

    build_obv(&f);
    wr16(f.buf + 4, 4);
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_VERSION);

    build_obv(&f);
    f.buf[0x08] ^= 0x08;  /* corrupt width, keep stale CRC */
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_CRC);

    build_obv(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_OK);
    f.buf[64 + 4] ^= 1;   /* corrupt index, header CRC still fine */
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, NFR * 8), OBV1_ERR_CRC);
}

static void test_obv_oversize(void)
{
    obv_file_t f;
    obv1_info_t i;
    /* header max_frame_size above the global cap */
    build_obv(&f);
    wr32(f.buf + 0x20, OBV1_FRAME_MAX + 1);
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_FRAME_SIZE);
    /* caller cap below max_frame_size */
    build_obv(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 59, &i), OBV1_ERR_FRAME_SIZE);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 60, &i), OBV1_OK);
    /* an index entry larger than the declared max_frame_size */
    build_obv(&f);
    wr32(f.buf + 0x20, 50);
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, NFR * 8), OBV1_ERR_FRAME_SIZE);
    /* max_frame_size 0 */
    build_obv(&f);
    wr32(f.buf + 0x20, 0);
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_FRAME_SIZE);
}

static void test_obv_geometry_index(void)
{
    obv_file_t f;
    obv1_info_t i;
    build_obv(&f);
    wr16(f.buf + 0x08, 244);   /* not a multiple of 8 */
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_GEOMETRY);
    build_obv(&f);
    wr16(f.buf + 0x0A, 368);   /* above 360 */
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_GEOMETRY);
    build_obv(&f);
    wr16(f.buf + 0x0C, 0);
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_GEOMETRY);
    build_obv(&f);
    f.buf[0x0E] = 4;
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_ERR_GEOMETRY);
    build_obv(&f);
    wr32(f.buf + 0x18, 64 + 8 * NFR + 4);
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, 0, 0, &i), OBV1_ERR_GEOMETRY);
    build_obv(&f);
    wr32(f.buf + 0x10, 0);
    fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, 0, 0, &i), OBV1_ERR_GEOMETRY);

    /* overlapping frames */
    build_obv(&f);
    wr32(f.buf + 64 + 8, 30);
    fix_index_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, NFR * 8), OBV1_ERR_INDEX);
    /* frame running past data_size */
    build_obv(&f);
    wr32(f.buf + 64 + 16, 70);
    fix_index_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, NFR * 8), OBV1_ERR_INDEX);
    /* offset overflow attempt */
    build_obv(&f);
    wr32(f.buf + 64 + 16, 0xFFFFFFF0u);
    fix_index_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, NFR * 8), OBV1_ERR_INDEX);
    /* tiny frame */
    build_obv(&f);
    wr32(f.buf + 64 + 12, 3);
    fix_index_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, f.len, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, NFR * 8), OBV1_ERR_INDEX);
    CHECK(strcmp(obv1_strerror(OBV1_ERR_MAGIC), "bad magic") == 0);
}

static void build_obv2(obv_file_t *f)
{
    memset(f, 0, sizeof(*f));
    uint8_t *h = f->buf;
    const uint32_t sizes[] = {40, 44, 48};
    const uint32_t times[] = {0, 33333, 66666};
    const uint32_t durations[] = {33333, 33333, 33334};
    const uint32_t count = 3;
    uint32_t data_off = OBV1_HEADER_SIZE + count * OBV2_INDEX_ENTRY;
    memcpy(h, "OBV1", 4);
    wr16(h + 4, OBV2_VERSION); wr16(h + 6, OBV1_HEADER_SIZE);
    wr16(h + 8, 360); wr16(h + 10, 360);
    h[0x0E] = OBV1_SUB_420; h[0x0F] = 0;
    wr32(h + 0x10, count); wr32(h + 0x14, OBV1_HEADER_SIZE);
    wr32(h + 0x18, data_off);
    wr32(h + 0x30, OBV2_TIMESCALE);
    uint32_t off = 0, maxsz = 0;
    for (uint32_t n = 0; n < count; n++) {
        uint8_t *e = h + OBV1_HEADER_SIZE + n * OBV2_INDEX_ENTRY;
        wr32(e, off); wr32(e + 4, sizes[n]); wr32(e + 8, times[n]); wr32(e + 12, durations[n]);
        uint8_t *fr = h + data_off + off;
        memset(fr, (int)(0x11 * (n + 1)), sizes[n]);
        fr[0] = 0xff; fr[1] = 0xd8; fr[sizes[n] - 2] = 0xff; fr[sizes[n] - 1] = 0xd9;
        off += sizes[n]; if (sizes[n] > maxsz) maxsz = sizes[n];
    }
    wr32(h + 0x1C, off); wr32(h + 0x20, maxsz); wr32(h + 0x24, 100);
    wr32(h + 0x2C, obv1_crc32(0, h + data_off, off));
    f->len = data_off + off;
    wr32(h + 0x28, obv1_crc32(0, h + OBV1_HEADER_SIZE, count * OBV2_INDEX_ENTRY));
    fix_header_crc(f);
}

static void test_obv2_timing(void)
{
    obv_file_t f; obv1_info_t i;
    build_obv2(&f);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(i.version, OBV2_VERSION); CHECK_EQ(i.fps, 0);
    CHECK_EQ(obv1_check_index(&i, f.buf + i.index_offset, f.len - i.index_offset), OBV1_OK);
    CHECK_EQ(obv_frame_time(f.buf + i.index_offset, 2), 66666);
    CHECK_EQ(obv_frame_duration(f.buf + i.index_offset, 2), 33334);
    CHECK_EQ(obv_timed_prebuffer(&i, f.buf + i.index_offset), 3);
    CHECK(obv_timed_ring_size(&i, f.buf + i.index_offset) > 0);
    CHECK_EQ(obv1_duration_ms(&i), 100);

    build_obv2(&f); wr32(f.buf + 64 + 16 + 8, 33334);
    wr32(f.buf + 0x28, obv1_crc32(0, f.buf + 64, 48)); fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, 48), OBV1_ERR_INDEX);

    build_obv2(&f); wr32(f.buf + 64 + 16 + 4, 43);
    wr32(f.buf + 0x28, obv1_crc32(0, f.buf + 64, 48)); fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, 48), OBV1_ERR_INDEX);

    build_obv2(&f); f.buf[15] = OBV1_FLAG_BLOCK_SAFE; fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len, 0, &i), OBV1_ERR_GEOMETRY);

    build_obv2(&f);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len + 1, 0, &i), OBV1_ERR_TRUNCATED);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len - 1, 0, &i), OBV1_ERR_TRUNCATED);
    wr16(f.buf + 10, 352); f.buf[15] = OBV1_FLAG_BLOCK_SAFE; fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len, 0, &i), OBV1_OK);
    build_obv2(&f); f.buf[14] = OBV1_SUB_422; f.buf[15] = OBV1_FLAG_BLOCK_SAFE; fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len, 0, &i), OBV1_OK);

    /* Valid CRC cannot conceal invalid timing or unused/extra data. */
    const uint32_t bad_at[] = {64 + 8, 64 + 12, 64 + 32 + 12, 64 + 32, 0x24};
    const uint32_t bad_value[] = {1, 16665, UINT32_MAX, 85, 101};
    for (size_t n = 0; n < sizeof(bad_at) / sizeof(bad_at[0]); n++) {
        build_obv2(&f); wr32(f.buf + bad_at[n], bad_value[n]);
        wr32(f.buf + 0x28, obv1_crc32(0, f.buf + 64, 48)); fix_header_crc(&f);
        CHECK_EQ(obv1_parse_header(f.buf, 64, f.len, 0, &i), OBV1_OK);
        CHECK_EQ(obv1_check_index(&i, f.buf + 64, 48), OBV1_ERR_INDEX);
    }
    build_obv2(&f);
    CHECK_EQ(obv1_parse_header(f.buf, 64, f.len, 0, &i), OBV1_OK);
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, 47), OBV1_ERR_TRUNCATED);
    f.buf[64] ^= 1;
    CHECK_EQ(obv1_check_index(&i, f.buf + 64, 48), OBV1_ERR_CRC);
}

static void build_obv3(obv_file_t *f)
{
    memset(f, 0, sizeof(*f));
    uint8_t *h = f->buf;
    memcpy(h, "OBV1", 4);
    wr16(h + 4, OBV3_VERSION); wr16(h + 6, OBV1_HEADER_SIZE);
    wr16(h + 8, 8); wr16(h + 10, 8); wr16(h + 12, 24);
    h[0x0E] = OBV1_SUB_420; h[0x0F] = OBV1_FLAG_LOOP;
    wr32(h + 0x10, 1); wr32(h + 0x14, OBV1_HEADER_SIZE);
    wr32(h + 0x18, OBV1_HEADER_SIZE + OBV1_INDEX_ENTRY);
    wr32(h + 0x1C, 14); wr32(h + 0x20, 14); wr32(h + 0x24, 42);
    uint8_t *entry = h + OBV1_HEADER_SIZE;
    wr32(entry, 0); wr32(entry + 4, 14);
    uint8_t *frame = h + OBV1_HEADER_SIZE + OBV1_INDEX_ENTRY;
    wr32(frame, 4); wr32(frame + 4, 2);
    frame[8] = 0xFF; frame[9] = 0xD8; frame[10] = 0xFF; frame[11] = 0xD9;
    frame[12] = 0xBD; frame[13] = 0x7F; /* 64 opaque pixels */
    wr32(h + 0x28, obv1_crc32(0, entry, OBV1_INDEX_ENTRY));
    wr32(h + 0x2C, obv1_crc32(0, frame, 14));
    f->len = OBV1_HEADER_SIZE + OBV1_INDEX_ENTRY + 14;
    fix_header_crc(f);
}

static void test_obv3_alpha(void)
{
    static const uint8_t encoded[] = {0xBD, 0x7F, 0x01, 0x00, 0xFF};
    uint8_t decoded[66];
    obv_file_t f;
    obv1_info_t info;
    CHECK(obv3_unpack_alpha(encoded, 2, decoded, 64));
    CHECK_EQ(decoded[0], 0x7F); CHECK_EQ(decoded[63], 0x7F);
    CHECK(!obv3_unpack_alpha(encoded, 1, decoded, 64));
    CHECK(!obv3_unpack_alpha(encoded, sizeof(encoded), decoded, 64));
    CHECK(obv3_unpack_alpha(encoded, sizeof(encoded), decoded, sizeof(decoded)));
    CHECK_EQ(decoded[64], 0x00); CHECK_EQ(decoded[65], 0xFF);

    /* A complete opaque plane can skip RGB565+alpha expansion. Classification
     * must reject malformed packets just as the unpacker does. */
    static const uint8_t opaque[] = {0xBD, 0xFF, 0x01, 0xFF, 0xFF};
    CHECK(obv3_alpha_is_opaque(opaque, 2, 64));
    CHECK(obv3_alpha_is_opaque(opaque, sizeof(opaque), 66));
    CHECK(!obv3_alpha_is_opaque(encoded, sizeof(encoded), 66));
    static const uint8_t mixed[] = {0xBD, 0xFF, 0x01, 0xFF, 0xFE};
    CHECK(!obv3_alpha_is_opaque(mixed, sizeof(mixed), 66));
    CHECK(!obv3_alpha_is_opaque(opaque, 1, 64));
    CHECK(!obv3_alpha_is_opaque(opaque, sizeof(opaque) - 1, 66));
    CHECK(!obv3_alpha_is_opaque(opaque, 2, 63));
    CHECK(!obv3_alpha_is_opaque(opaque, 2, 65));
    CHECK(!obv3_alpha_is_opaque(opaque, sizeof(opaque), 64));
    CHECK(!obv3_alpha_is_opaque(NULL, 2, 64));
    CHECK(!obv3_alpha_is_opaque(opaque, 0, 64));
    CHECK(!obv3_alpha_is_opaque(opaque, 2, 0));

    build_obv3(&f);
    CHECK_EQ(obv1_parse_header(f.buf, OBV1_HEADER_SIZE, f.len, 0, &info), OBV1_OK);
    CHECK_EQ(info.version, OBV3_VERSION);
    CHECK_EQ(obv1_check_index(&info, f.buf + info.index_offset, f.len - info.index_offset), OBV1_OK);
    f.buf[0x0F] = 0; fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, OBV1_HEADER_SIZE, f.len, 0, &info), OBV1_ERR_GEOMETRY);
    build_obv3(&f);
    wr32(f.buf + 64, 1); wr32(f.buf + 0x28, obv1_crc32(0, f.buf + 64, 8)); fix_header_crc(&f);
    CHECK_EQ(obv1_parse_header(f.buf, OBV1_HEADER_SIZE, f.len, 0, &info), OBV1_OK);
    CHECK_EQ(obv1_check_index(&info, f.buf + 64, f.len - 64), OBV1_ERR_INDEX);
}

static void test_obv2_prebuffer_budget(void)
{
    uint8_t index[61 * OBV2_INDEX_ENTRY] = {0};
    obv1_info_t i = {.version = OBV2_VERSION, .frame_count = 61,
        .max_frame_size = 1000, .data_size = 61000, .duration_ms = 1017};
    for (uint32_t n = 0; n < i.frame_count; n++) {
        wr32(index + n * 16, n * 1000); wr32(index + n * 16 + 4, 1000);
        wr32(index + n * 16 + 8, n * 16667); wr32(index + n * 16 + 12, 16667);
    }
    i.index_crc32 = obv1_crc32(0, index, sizeof(index));
    CHECK_EQ(obv1_check_index(&i, index, sizeof(index)), OBV1_OK);
    CHECK_EQ(obv_timed_prebuffer(&i, index), 60);
    CHECK_EQ(obv_timed_ring_size(&i, index), 62 * 1008);
    /* A legal per-frame size can still exceed the one-second memory budget. */
    i.max_frame_size = OBV1_FRAME_MAX; i.data_size = i.frame_count * OBV1_FRAME_MAX;
    for (uint32_t n = 0; n < i.frame_count; n++) {
        wr32(index + n * 16, n * OBV1_FRAME_MAX); wr32(index + n * 16 + 4, OBV1_FRAME_MAX);
    }
    i.index_crc32 = obv1_crc32(0, index, sizeof(index));
    CHECK_EQ(obv_timed_ring_size(&i, index), 0);
    CHECK_EQ(obv1_check_index(&i, index, sizeof(index)), OBV1_ERR_INDEX);

    /* Variable intervals use presentation time rather than an average FPS. */
    i.frame_count = 3; i.max_frame_size = 1000;
    wr32(index + 8, 0); wr32(index + 12, 100000);
    wr32(index + 24, 100000); wr32(index + 28, 900000);
    wr32(index + 40, 1000000); wr32(index + 44, 600000);
    for (uint32_t n = 0; n < i.frame_count; n++) wr32(index + n * 16 + 4, 1000);
    CHECK_EQ(obv_timed_prebuffer(&i, index), 2);
    CHECK_EQ(obv_timed_ring_size(&i, index), 4 * 1008);
}

/* ------------------------------------------------------------ delta_varint */

typedef struct {
    const uint8_t *data;
    size_t len;
    unsigned reads;
} mem_src_t;

static size_t mem_read(void *ctx, uint32_t off, uint8_t *dst, size_t len)
{
    mem_src_t *s = (mem_src_t *)ctx;
    s->reads++;
    if (off >= s->len) return 0;
    if (len > s->len - off) len = s->len - off;
    memcpy(dst, s->data + off, len);
    return len;
}

static const char k_manifest_v1[] =
    "canvas_width=8\ncanvas_height=8\ngrid_width=4\r\ngrid_height=4\n"
    "fps=10\nframe_count=2\nstream_format=delta_varint_rgb565_black_v1\n"
    "data_file=boot_block.bin\n";

static void test_dv_manifest(void)
{
    dv_manifest_t m;
    char buf[512];
    size_t n = strlen(k_manifest_v1);
    memcpy(buf, k_manifest_v1, n);
    memset(buf + n, 0xFF, sizeof(buf) - n);   /* flash padding, no NUL */
    CHECK_EQ(dv_parse_manifest(buf, sizeof(buf), &m), DV_OK);
    CHECK_EQ(m.canvas_w, 8);
    CHECK_EQ(m.grid_w, 4);
    CHECK_EQ(m.grid_h, 4);
    CHECK_EQ(m.fps, 10);
    CHECK_EQ(m.frame_count, 2);
    CHECK_EQ(m.duration_ms, 200);   /* default frame_count * 1000 / fps */
    CHECK_EQ(m.binary_size, 0);
    CHECK(!m.count_u32);

    const char *v2 = "canvas_width=360\ncanvas_height=360\ngrid_width=360\ngrid_height=360\n"
                     "fps=15\nframe_count=37\nduration_ms=2467\nbinary_size=1000\n"
                     "stream_format=delta_varint_rgb565_black_v2";
    CHECK_EQ(dv_parse_manifest(v2, strlen(v2), &m), DV_OK);
    CHECK(m.count_u32);
    CHECK_EQ(m.duration_ms, 2467);
    CHECK_EQ(m.binary_size, 1000);

    /* A trimmed/re-encoded stream must not hold its last frame for an old
     * source duration, nor finish early for a stale shorter duration. */
    const char *stale_long = "canvas_width=8\ncanvas_height=8\ngrid_width=4\ngrid_height=4\n"
                             "fps=30\nframe_count=178\nduration_ms=30000\n";
    CHECK_EQ(dv_parse_manifest(stale_long, strlen(stale_long), &m), DV_OK);
    CHECK_EQ(m.duration_ms, 5933);
    const char *stale_short = "canvas_width=8\ncanvas_height=8\ngrid_width=4\ngrid_height=4\n"
                              "fps=30\nframe_count=178\nduration_ms=100\n";
    CHECK_EQ(dv_parse_manifest(stale_short, strlen(stale_short), &m), DV_OK);
    CHECK_EQ(m.duration_ms, 5933);
    const char *overflow = "canvas_width=8\ncanvas_height=8\ngrid_width=4\ngrid_height=4\n"
                           "fps=1\nframe_count=4294967295\n";
    CHECK_EQ(dv_parse_manifest(overflow, strlen(overflow), &m), DV_ERR_MANIFEST);

    const char *bad_fmt = "canvas_width=8\ncanvas_height=8\ngrid_width=4\ngrid_height=4\n"
                          "fps=10\nframe_count=2\nstream_format=rgb565_raw\n";
    CHECK_EQ(dv_parse_manifest(bad_fmt, strlen(bad_fmt), &m), DV_ERR_MANIFEST);
    const char *missing = "canvas_width=8\ncanvas_height=8\ngrid_width=4\nfps=10\nframe_count=2\n";
    CHECK_EQ(dv_parse_manifest(missing, strlen(missing), &m), DV_ERR_MANIFEST);
    const char *huge = "canvas_width=1024\ncanvas_height=8\ngrid_width=4\ngrid_height=4\n"
                       "fps=10\nframe_count=2\n";
    CHECK_EQ(dv_parse_manifest(huge, strlen(huge), &m), DV_ERR_MANIFEST);
    CHECK_EQ(dv_parse_manifest("\xff\xff\xff", 3, &m), DV_ERR_MANIFEST);
}

/* 8x8 canvas, 4x4 grid (2x2 px cells), two v1 frames. */
static const uint8_t k_stream_v1[] = {
    /* frame 0: 3 changes */
    0x03, 0x00,
    0x00, 0x00, 0xF8,         /* idx 0, red 0xF800 */
    0x0B,                     /* delta 5 -> idx 5, black */
    0x14, 0x1F, 0x00,         /* delta 10 -> idx 15, blue 0x001F */
    /* frame 1: 1 change */
    0x01, 0x00,
    0x06, 0xE0, 0x07,         /* idx 3, green 0x07E0 */
};

static void test_dv_stream(void)
{
    dv_manifest_t m;
    CHECK_EQ(dv_parse_manifest(k_manifest_v1, strlen(k_manifest_v1), &m), DV_OK);
    mem_src_t src = {k_stream_v1, sizeof(k_stream_v1), 0};
    uint8_t chunk[3];   /* tiny chunk: every field straddles a refill */
    uint16_t xe[5], ye[5], px[64];
    dv_stream_t s;
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &src, sizeof(k_stream_v1), chunk, sizeof(chunk),
                            xe, ye, px, false), DV_OK);
    CHECK_EQ(xe[4], 8);
    CHECK_EQ(ye[2], 4);

    CHECK_EQ(dv_stream_next_frame(&s), DV_OK);
    CHECK_EQ(px[0], 0xF800);
    CHECK_EQ(px[1 * 8 + 1], 0xF800);
    CHECK_EQ(px[2], 0);
    CHECK_EQ(px[7 * 8 + 7], 0x001F);
    CHECK_EQ(px[6 * 8 + 6], 0x001F);
    CHECK_EQ(s.dirty_x0, 0);
    CHECK_EQ(s.dirty_y0, 0);
    CHECK_EQ(s.dirty_x1, 8);
    CHECK_EQ(s.dirty_y1, 8);

    CHECK_EQ(dv_stream_next_frame(&s), DV_OK);
    CHECK_EQ(px[6], 0x07E0);
    CHECK_EQ(px[1 * 8 + 7], 0x07E0);
    CHECK_EQ(px[0], 0xF800);            /* previous frame kept */
    CHECK_EQ(s.dirty_x0, 6);
    CHECK_EQ(s.dirty_y0, 0);
    CHECK_EQ(s.dirty_x1, 8);
    CHECK_EQ(s.dirty_y1, 2);
    CHECK_EQ(s.frame, 2);
    CHECK(src.reads >= 5);

    /* past the end */
    CHECK_EQ(dv_stream_next_frame(&s), DV_ERR_EOF);

    /* byte-swapped output (LV_COLOR_16_SWAP) */
    src.reads = 0;
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &src, sizeof(k_stream_v1), chunk, sizeof(chunk),
                            xe, ye, px, true), DV_OK);
    CHECK_EQ(px[0], 0);
    CHECK_EQ(dv_stream_next_frame(&s), DV_OK);
    CHECK_EQ(px[0], 0x00F8);
    CHECK_EQ(px[63], 0x1F00);
}

static void test_dv_errors(void)
{
    dv_manifest_t m;
    CHECK_EQ(dv_parse_manifest(k_manifest_v1, strlen(k_manifest_v1), &m), DV_OK);
    uint8_t chunk[16];
    uint16_t xe[5], ye[5], px[64];
    dv_stream_t s;

    /* truncated inside frame 0 */
    mem_src_t src = {k_stream_v1, 6, 0};
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &src, 6, chunk, sizeof(chunk), xe, ye, px, false), DV_OK);
    CHECK_EQ(dv_stream_next_frame(&s), DV_ERR_EOF);

    /* binary_size limits the read even when more data is there */
    m.binary_size = 9;
    src.len = sizeof(k_stream_v1);
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &src, sizeof(k_stream_v1), chunk, sizeof(chunk),
                            xe, ye, px, false), DV_OK);
    CHECK_EQ(dv_stream_next_frame(&s), DV_OK);
    CHECK_EQ(dv_stream_next_frame(&s), DV_ERR_EOF);
    /* binary_size larger than what the partition / file can hold */
    m.binary_size = 1000;
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &src, 999, chunk, sizeof(chunk), xe, ye, px, false),
             DV_ERR_MANIFEST);
    m.binary_size = 0;

    /* cell index out of range (16 cells) */
    static const uint8_t oob[] = {0x01, 0x00, 0x21};   /* idx 16, black */
    mem_src_t s2 = {oob, sizeof(oob), 0};
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &s2, sizeof(oob), chunk, sizeof(chunk), xe, ye, px, false), DV_OK);
    CHECK_EQ(dv_stream_next_frame(&s), DV_ERR_DATA);

    /* varint longer than 4 bytes */
    static const uint8_t longv[] = {0x01, 0x00, 0x80, 0x80, 0x80, 0x80, 0x01};
    mem_src_t s3 = {longv, sizeof(longv), 0};
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &s3, sizeof(longv), chunk, sizeof(chunk), xe, ye, px, false), DV_OK);
    CHECK_EQ(dv_stream_next_frame(&s), DV_ERR_DATA);

    /* v2: u32 change count */
    m.count_u32 = true;
    static const uint8_t v2[] = {0x01, 0x00, 0x00, 0x00, 0x1E, 0x1F, 0x00};  /* idx 15 blue */
    mem_src_t s4 = {v2, sizeof(v2), 0};
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &s4, sizeof(v2), chunk, sizeof(chunk), xe, ye, px, false), DV_OK);
    CHECK_EQ(dv_stream_next_frame(&s), DV_OK);
    CHECK_EQ(px[63], 0x001F);
}

/* Real asset: every frame decodes and the stream is consumed exactly. */
static void test_dv_slot_a(const char *root)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/bootmedia/slot_a/boot_block.txt", root);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip slot_a round trip (%s missing)\n", path);
        return;
    }
    char text[4096];
    size_t tl = fread(text, 1, sizeof(text), f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/bootmedia/slot_a/boot_block.bin", root);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long bl = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bin = malloc((size_t)bl);
    CHECK(bin && fread(bin, 1, (size_t)bl, f) == (size_t)bl);
    fclose(f);

    dv_manifest_t m;
    CHECK_EQ(dv_parse_manifest(text, tl, &m), DV_OK);
    uint8_t *chunk = malloc(16 * 1024);
    uint16_t *xe = malloc((m.grid_w + 1u) * sizeof(uint16_t));
    uint16_t *ye = malloc((m.grid_h + 1u) * sizeof(uint16_t));
    uint16_t *px = malloc((size_t)m.canvas_w * m.canvas_h * sizeof(uint16_t));
    mem_src_t src = {bin, (size_t)bl, 0};
    dv_stream_t s;
    CHECK_EQ(dv_stream_init(&s, &m, mem_read, &src, (uint32_t)bl + 4096u, chunk, 16 * 1024,
                            xe, ye, px, true), DV_OK);
    for (uint32_t n = 0; n < m.frame_count; n++) {
        int err = dv_stream_next_frame(&s);
        if (err != DV_OK) {
            CHECK_EQ(err, DV_OK);
            break;
        }
    }
    CHECK_EQ(s.frame, m.frame_count);
    CHECK_EQ(s.next_off - (s.chunk_len - s.chunk_pos), (uint32_t)bl);
    printf("slot_a: %ux%u grid %ux%u, %u frames, %ld B, %u reads of 16 KB\n",
           m.canvas_w, m.canvas_h, m.grid_w, m.grid_h, (unsigned)m.frame_count, bl, src.reads);
    free(px); free(ye); free(xe); free(chunk); free(bin);
}

int main(int argc, char **argv)
{
    test_crc();
    test_obv_ok();
    test_obv_truncated();
    test_obv_bad_magic_version_crc();
    test_obv_oversize();
    test_obv_geometry_index();
    test_obv2_timing();
    test_obv3_alpha();
    test_obv2_prebuffer_budget();
    test_dv_manifest();
    test_dv_stream();
    test_dv_errors();
    if (argc > 1) test_dv_slot_a(argv[1]);

    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
