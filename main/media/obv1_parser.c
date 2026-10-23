/*
 * obv1_parser.c — OBV1 container header / index validation (pure C, host-testable).
 * Format: see obv1_format.h.
 */

#include "obv1_format.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#endif

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t obv1_crc32(uint32_t crc, const void *data, size_t len)
{
#ifdef ESP_PLATFORM
    /* ESP32 size_t and the ROM length are both 32-bit. Preserve empty-input
     * semantics without passing a possibly NULL pointer to the ROM API. */
    if (!len) return crc;
    return esp_rom_crc32_le(crc, (const uint8_t *)data, (uint32_t)len);
#else
    /* Small portable fallback for host tools; the device uses ROM above. */
    static const uint32_t tab[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ tab[crc & 0x0F];
        crc = (crc >> 4) ^ tab[crc & 0x0F];
    }
    return ~crc;
#endif
}

int obv1_parse_header(const uint8_t *buf, size_t len, uint64_t file_size,
                      uint32_t frame_cap, obv1_info_t *out)
{
    if (!buf || !out) return OBV1_ERR_TRUNCATED;
    if (len < OBV1_HEADER_SIZE) return OBV1_ERR_TRUNCATED;
    if (file_size && file_size < OBV1_HEADER_SIZE) return OBV1_ERR_TRUNCATED;
    if (memcmp(buf, "OBV1", 4) != 0) return OBV1_ERR_MAGIC;
    uint16_t version = rd16(buf + 0x04);
    if ((version != OBV1_VERSION && version != OBV2_VERSION && version != OBV3_VERSION) || rd16(buf + 0x06) != OBV1_HEADER_SIZE) {
        return OBV1_ERR_VERSION;
    }
    if (obv1_crc32(0, buf, 0x3C) != rd32(buf + 0x3C)) return OBV1_ERR_CRC;

    obv1_info_t i;
    memset(&i, 0, sizeof(i));
    i.version = rd16(buf + 0x04);
    i.width = rd16(buf + 0x08);
    i.height = rd16(buf + 0x0A);
    i.fps = rd16(buf + 0x0C);
    i.subsampling = buf[0x0E];
    i.flags = buf[0x0F];
    i.frame_count = rd32(buf + 0x10);
    i.index_offset = rd32(buf + 0x14);
    i.data_offset = rd32(buf + 0x18);
    i.data_size = rd32(buf + 0x1C);
    i.max_frame_size = rd32(buf + 0x20);
    i.duration_ms = rd32(buf + 0x24);
    i.index_crc32 = rd32(buf + 0x28);
    i.data_crc32 = rd32(buf + 0x2C);

    if (i.width < 8 || i.height < 8 || i.width > OBV1_MAX_DIM || i.height > OBV1_MAX_DIM ||
        (i.width & 7) || (i.height & 7) ||
        (i.version != OBV2_VERSION ? (i.fps < 1 || i.fps > 60) :
         (i.fps != 0 || rd32(buf + 0x30) != OBV2_TIMESCALE || rd32(buf + 0x34) || rd32(buf + 0x38))) ||
        i.subsampling > OBV1_SUB_420) {
        return OBV1_ERR_GEOMETRY;
    }
    if (i.frame_count < 1 || i.frame_count > OBV1_MAX_FRAMES) return OBV1_ERR_GEOMETRY;
    if (i.index_offset != OBV1_HEADER_SIZE ||
        i.data_offset != OBV1_HEADER_SIZE + obv_index_stride(&i) * i.frame_count) {
        return OBV1_ERR_GEOMETRY;
    }

    uint32_t format_cap = i.version == OBV3_VERSION ? OBV3_FRAME_MAX : OBV1_FRAME_MAX;
    uint32_t cap = frame_cap ? frame_cap : format_cap;
    if (cap > format_cap) cap = format_cap;
    if (i.max_frame_size < 4 || i.max_frame_size > cap) return OBV1_ERR_FRAME_SIZE;
    if (i.data_size < (i.version == OBV3_VERSION ? 13u : 4u)) return OBV1_ERR_INDEX;

    if (file_size && (file_size < (uint64_t)i.data_offset + i.data_size ||
        (i.version == OBV2_VERSION && file_size != (uint64_t)i.data_offset + i.data_size))) return OBV1_ERR_TRUNCATED;
    if (i.version == OBV3_VERSION &&
        (i.subsampling != OBV1_SUB_420 || i.flags != OBV1_FLAG_LOOP ||
         rd32(buf + 0x30) || rd32(buf + 0x34) || rd32(buf + 0x38)))
        return OBV1_ERR_GEOMETRY;
    if (i.version == OBV2_VERSION &&
        ((i.subsampling != OBV1_SUB_420 && i.subsampling != OBV1_SUB_422) ||
         i.flags != (i.subsampling == OBV1_SUB_422 || !(i.height & 15) ? OBV1_FLAG_BLOCK_SAFE : 0)))
        return OBV1_ERR_GEOMETRY;

    *out = i;
    return OBV1_OK;
}

void obv1_index_entry(const uint8_t *index, uint32_t i, uint32_t *offset, uint32_t *size)
{
    const uint8_t *e = index + (size_t)i * OBV1_INDEX_ENTRY;
    if (offset) *offset = rd32(e);
    if (size) *size = rd32(e + 4);
}

int obv1_check_index(const obv1_info_t *info, const uint8_t *index, size_t len)
{
    if (!info || !index) return OBV1_ERR_TRUNCATED;
    size_t need = (size_t)info->frame_count * obv_index_stride(info);
    if (len < need) return OBV1_ERR_TRUNCATED;
    if (obv1_crc32(0, index, need) != info->index_crc32) return OBV1_ERR_CRC;

    uint32_t next = 0;  /* first byte not covered by the previous frame */
    for (uint32_t n = 0; n < info->frame_count; n++) {
        uint32_t off, size;
        obv_index_frame(info, index, n, &off, &size);
        if (size > info->max_frame_size) return OBV1_ERR_FRAME_SIZE;
        if (size < (info->version == OBV3_VERSION ? 13u : 4u) ||
            (info->version != OBV1_VERSION ? off != next : off < next) ||
            off > info->data_size || size > info->data_size - off) {
            return OBV1_ERR_INDEX;
        }
        next = off + size;
    }
    if (info->version == OBV2_VERSION) {
        uint64_t end = 0;
        for (uint32_t n = 0; n < info->frame_count; n++) {
            uint32_t t = obv_frame_time(index, n), d = obv_frame_duration(index, n);
            if (t != end || d < 16666u || (end += d) > UINT32_MAX) return OBV1_ERR_INDEX;
        }
        if (next != info->data_size) return OBV1_ERR_INDEX;
        if ((end + 999u) / 1000u != info->duration_ms || !obv_timed_ring_size(info, index)) return OBV1_ERR_INDEX;
    } else if (info->version == OBV3_VERSION && next != info->data_size) return OBV1_ERR_INDEX;
    return OBV1_OK;
}

uint32_t obv_index_stride(const obv1_info_t *i)
{ return i->version == OBV2_VERSION ? OBV2_INDEX_ENTRY : OBV1_INDEX_ENTRY; }
void obv_index_frame(const obv1_info_t *i, const uint8_t *p, uint32_t n, uint32_t *off, uint32_t *size)
{
    p += (size_t)n * obv_index_stride(i);
    if (off) *off = rd32(p);
    if (size) *size = rd32(p + 4);
}
uint32_t obv_frame_time(const uint8_t *p, uint32_t n) { return rd32(p + (size_t)n * 16 + 8); }
uint32_t obv_frame_duration(const uint8_t *p, uint32_t n) { return rd32(p + (size_t)n * 16 + 12); }
/* Decode and/or classify with the same strict packet/plane validation. */
static bool alpha_plane(const uint8_t *src, size_t src_len, uint8_t *dst,
                        size_t dst_len, bool *opaque)
{
    if (!src || !src_len || !dst_len) return false;
    bool all_opaque = true;
    size_t in = 0, out = 0;
    while (in < src_len && out < dst_len) {
        uint8_t control = src[in++];
        if (control < 128) {
            size_t count = (size_t)control + 1u;
            if (count > src_len - in || count > dst_len - out) return false;
            if (opaque && all_opaque) {
                for (size_t n = 0; n < count; n++) {
                    if (src[in + n] != 255) { all_opaque = false; break; }
                }
            }
            if (dst) memcpy(dst + out, src + in, count);
            in += count;
            out += count;
        } else {
            size_t count = (size_t)(control & 127u) + 3u;
            if (in >= src_len || count > dst_len - out) return false;
            uint8_t value = src[in++];
            if (value != 255) all_opaque = false;
            if (dst) memset(dst + out, value, count);
            out += count;
        }
    }
    if (in != src_len || out != dst_len) return false;
    if (opaque) *opaque = all_opaque;
    return true;
}
bool obv3_unpack_alpha(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
    return dst && alpha_plane(src, src_len, dst, dst_len, NULL);
}
bool obv3_alpha_is_opaque(const uint8_t *src, size_t src_len, size_t pixels)
{
    bool opaque = false;
    return alpha_plane(src, src_len, NULL, pixels, &opaque) && opaque;
}
static uint32_t record_size(const uint8_t *p, uint32_t n)
{ return 8 + ((rd32(p + (size_t)n * 16 + 4) + 3u) & ~3u); }
uint32_t obv_timed_ring_size(const obv1_info_t *i, const uint8_t *p)
{
    uint64_t sum = 0, largest = 0;
    uint32_t left = 0;
    for (uint32_t n = 0; n < i->frame_count; n++) {
        sum += record_size(p, n);
        while (left < n && (uint64_t)obv_frame_time(p, n) - obv_frame_time(p, left) >= 1000000u)
            sum -= record_size(p, left++);
        if (sum > largest) largest = sum;
    }
    uint64_t want = largest + 2u * (8u + ((i->max_frame_size + 3u) & ~3u));
    return want <= OBV2_RING_MAX ? (uint32_t)want : 0;
}
uint32_t obv_timed_prebuffer(const obv1_info_t *i, const uint8_t *p)
{
    for (uint32_t n = 0; n < i->frame_count; n++)
        if ((uint64_t)obv_frame_time(p, n) + obv_frame_duration(p, n) >= 1000000u) return n + 1;
    return i->frame_count;
}

uint32_t obv1_duration_ms(const obv1_info_t *info)
{
    if (!info) return 0;
    if (info->version == OBV2_VERSION) return info->duration_ms;
    if (!info->fps) return 0;
    return (uint32_t)(((uint64_t)info->frame_count * 1000u) / info->fps);
}

const char *obv1_strerror(int err)
{
    switch (err) {
    case OBV1_OK:             return "ok";
    case OBV1_ERR_TRUNCATED:  return "truncated";
    case OBV1_ERR_MAGIC:      return "bad magic";
    case OBV1_ERR_VERSION:    return "unsupported version";
    case OBV1_ERR_CRC:        return "crc mismatch";
    case OBV1_ERR_GEOMETRY:   return "bad geometry";
    case OBV1_ERR_FRAME_SIZE: return "frame too large";
    case OBV1_ERR_INDEX:      return "bad frame index";
    default:                  return "unknown";
    }
}
