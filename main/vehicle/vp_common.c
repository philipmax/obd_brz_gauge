#include "vp_common.h"

#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool vp_parse_hex_id(const char *s, uint32_t *out, size_t *ndigits)
{
    uint32_t v = 0;
    size_t n = 0;

    if (!s || !out) return false;
    for (; s[n]; n++) {
        int h = hex_nibble(s[n]);
        if (h < 0 || n >= 8) return false;
        v = (v << 4) | (uint32_t)h;
    }
    if (n == 0) return false;
    *out = v;
    if (ndigits) *ndigits = n;
    return true;
}

bool vp_parse_hex_bytes(const char *s, bool allow_spaces, uint8_t *out, size_t cap, size_t *n)
{
    size_t cnt = 0;
    const char *p = s;

    if (!s || !out || !n) return false;
    while (*p) {
        int hi, lo;
        if (cnt > 0 && allow_spaces && *p == ' ') {
            p++;
        }
        hi = hex_nibble(p[0]);
        if (hi < 0) return false;
        lo = hex_nibble(p[1]);
        if (lo < 0) return false;
        if (cnt >= cap) return false;
        out[cnt++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    if (cnt == 0) return false;
    *n = cnt;
    return true;
}

vp_hdr_class_t vp_classify_tx_id(uint32_t id)
{
    if (id & VP_ID_29BIT) {
        uint32_t v = id & VP_ID_MASK;
        if (v == 0x18DB33F1u) return VP_HDR_FUNCTIONAL;
        if ((v & 0xFFFF00FFu) == 0x18DA00F1u) return VP_HDR_PHYSICAL;
        return VP_HDR_INVALID;
    }
    if (id == 0x7DFu) return VP_HDR_FUNCTIONAL;
    if (id >= 0x7E0u && id <= 0x7E7u) return VP_HDR_PHYSICAL;
    if (id >= 0x600u && id <= 0x6FFu) return VP_HDR_PHYSICAL;
    return VP_HDR_INVALID;
}

vp_hdr_class_t vp_classify_tx_header(const char *s, uint32_t *id)
{
    uint32_t v;
    size_t nd;
    vp_hdr_class_t c;

    if (!vp_parse_hex_id(s, &v, &nd)) return VP_HDR_INVALID;
    if (nd == 3) {
        /* 3 digits */
    } else if (nd == 8) {
        if (v > VP_ID_MASK) return VP_HDR_INVALID;
        v |= VP_ID_29BIT;
    } else {
        return VP_HDR_INVALID;
    }
    c = vp_classify_tx_id(v);
    if (c != VP_HDR_INVALID && id) *id = v;
    return c;
}

bool vp_parse_rx_filter(const char *s, uint32_t *id)
{
    uint32_t v;
    size_t nd;

    if (!vp_parse_hex_id(s, &v, &nd)) return false;
    if (nd == 3) {
        if (!((v >= 0x7E8u && v <= 0x7EFu) || (v >= 0x600u && v <= 0x6FFu))) return false;
    } else if (nd == 8) {
        if ((v & 0xFFFFFF00u) != 0x18DAF100u) return false;
        v |= VP_ID_29BIT;
    } else {
        return false;
    }
    if (id) *id = v;
    return true;
}

bool vp_parse_can_id(const char *s, uint32_t *id)
{
    uint32_t v;
    size_t nd;

    if (!vp_parse_hex_id(s, &v, &nd)) return false;
    if (nd == 3) {
        if (v > 0x7FFu) return false;
    } else if (nd == 8) {
        if (v > VP_ID_MASK) return false;
        v |= VP_ID_29BIT;
    } else {
        return false;
    }
    if (id) *id = v;
    return true;
}

bool vp_service_allowed(uint8_t svc)
{
    return svc == 0x01 || svc == 0x09 || svc == 0x21 || svc == 0x22;
}

bool vp_service_functional_ok(uint8_t svc)
{
    return svc == 0x01 || svc == 0x09;
}

bool vp_service_len_ok(uint8_t svc, size_t len)
{
    switch (svc) {
    case 0x01:
    case 0x09:
    case 0x21:
        return len == 2;
    case 0x22:
        return len == 3;
    default:
        return false;
    }
}

size_t vp_rawframe_parse(const uint8_t *b, size_t n, vp_rawframe_t out[2])
{
    size_t k = 0;

    if (!b || !out || n == 0 || n > 8) return 0;
    /* normal addressing: PCI 0x0L, L = remaining bytes, 1..7 */
    if ((b[0] & 0xF0u) == 0x00u && (b[0] & 0x0Fu) >= 1 && (size_t)(b[0] & 0x0Fu) == n - 1) {
        out[k].ext = false;
        out[k].ext_addr = 0;
        out[k].off = 1;
        out[k].len = (uint8_t)(n - 1);
        k++;
    }
    /* extended addressing: target byte, then PCI 0x0L, L = remaining, 1..6 */
    if (n >= 3 && (b[1] & 0xF0u) == 0x00u && (b[1] & 0x0Fu) >= 1 &&
        (size_t)(b[1] & 0x0Fu) == n - 2) {
        out[k].ext = true;
        out[k].ext_addr = b[0];
        out[k].off = 2;
        out[k].len = (uint8_t)(n - 2);
        k++;
    }
    return k;
}

bool vp_id_ok(const char *s)
{
    size_t n = 0;

    if (!s) return false;
    for (; s[n]; n++) {
        char c = s[n];
        if (n >= 31) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return n > 0;
}

bool vp_ascii_ok(const char *s, size_t max)
{
    size_t n = 0;

    if (!s) return false;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (n >= max || c < 0x20 || c > 0x7E) return false;
    }
    return n > 0;
}

uint32_t vp_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    int b;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
