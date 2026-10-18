/*
 * delta_varint.c — streaming decoder for the legacy boot animation
 * (delta_varint_rgb565_black_v1 / v2). Format: see delta_varint.h.
 */

#include "delta_varint.h"

#include <stdlib.h>
#include <string.h>

#define DV_FMT_V1 "delta_varint_rgb565_black_v1"
#define DV_FMT_V2 "delta_varint_rgb565_black_v2"

static bool parse_u32(const char *v, uint32_t *out)
{
    char *end = NULL;
    unsigned long x = strtoul(v, &end, 10);
    if (end == v) return false;
    *out = (uint32_t)x;
    return true;
}

int dv_parse_manifest(const char *text, size_t len, dv_manifest_t *out)
{
    if (!text || !out) return DV_ERR_MANIFEST;
    dv_manifest_t m;
    memset(&m, 0, sizeof(m));
    bool fmt_ok = true;

    size_t pos = 0;
    while (pos < len) {
        unsigned char c0 = (unsigned char)text[pos];
        if (c0 == 0x00 || c0 == 0xFF) break;   /* end of text / flash padding */
        size_t end = pos;
        while (end < len && text[end] != '\n' && text[end] != '\0' &&
               (unsigned char)text[end] != 0xFF) {
            end++;
        }
        char line[128];
        size_t n = end - pos;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, text + pos, n);
        line[n] = '\0';
        pos = (end < len && text[end] == '\n') ? end + 1 : end;

        char *val = strchr(line, '=');
        if (!val) continue;
        *val++ = '\0';
        line[strcspn(line, "\r\t ")] = '\0';
        val[strcspn(val, "\r\n")] = '\0';

        uint32_t x = 0;
        if (strcmp(line, "canvas_width") == 0 && parse_u32(val, &x)) m.canvas_w = (uint16_t)(x > 0xFFFF ? 0xFFFF : x);
        else if (strcmp(line, "canvas_height") == 0 && parse_u32(val, &x)) m.canvas_h = (uint16_t)(x > 0xFFFF ? 0xFFFF : x);
        else if (strcmp(line, "grid_width") == 0 && parse_u32(val, &x)) m.grid_w = (uint16_t)(x > 0xFFFF ? 0xFFFF : x);
        else if (strcmp(line, "grid_height") == 0 && parse_u32(val, &x)) m.grid_h = (uint16_t)(x > 0xFFFF ? 0xFFFF : x);
        else if (strcmp(line, "fps") == 0 && parse_u32(val, &x)) m.fps = (uint16_t)(x > 0xFFFF ? 0xFFFF : x);
        else if (strcmp(line, "frame_count") == 0 && parse_u32(val, &x)) m.frame_count = x;
        else if (strcmp(line, "binary_size") == 0 && parse_u32(val, &x)) m.binary_size = x;
        else if (strcmp(line, "stream_format") == 0) {
            if (strcmp(val, DV_FMT_V1) == 0) m.count_u32 = false;
            else if (strcmp(val, DV_FMT_V2) == 0) m.count_u32 = true;
            else fmt_ok = false;
        }
    }

    if (!fmt_ok || !m.canvas_w || !m.canvas_h || !m.grid_w || !m.grid_h ||
        !m.fps || m.fps > 120 || !m.frame_count) {
        return DV_ERR_MANIFEST;
    }
    if (m.canvas_w > DV_MAX_DIM || m.canvas_h > DV_MAX_DIM ||
        m.grid_w > DV_MAX_DIM || m.grid_h > DV_MAX_DIM) {
        return DV_ERR_MANIFEST;
    }
    /* Frame count and FPS define the presentation timeline; duration_ms is
     * legacy metadata and may not match the encoded frame stream. */
    uint64_t duration = ((uint64_t)m.frame_count * 1000u + m.fps / 2u) / m.fps;
    if (duration > UINT32_MAX) return DV_ERR_MANIFEST;
    m.duration_ms = (uint32_t)duration;
    *out = m;
    return DV_OK;
}

int dv_stream_init(dv_stream_t *s, const dv_manifest_t *m, dv_read_fn read, void *ctx,
                   uint32_t limit, uint8_t *chunk, size_t chunk_cap,
                   uint16_t *x_edges, uint16_t *y_edges, uint16_t *pixels, bool swap)
{
    if (!s || !m || !read || !chunk || chunk_cap == 0 || !x_edges || !y_edges || !pixels) {
        return DV_ERR_MANIFEST;
    }
    if (m->binary_size) {
        if (m->binary_size > limit) return DV_ERR_MANIFEST;
        limit = m->binary_size;
    }
    memset(s, 0, sizeof(*s));
    s->m = *m;
    s->read = read;
    s->ctx = ctx;
    s->chunk = chunk;
    s->chunk_cap = chunk_cap;
    s->limit = limit;
    s->x_edges = x_edges;
    s->y_edges = y_edges;
    s->pixels = pixels;
    s->swap = swap;
    for (uint32_t i = 0; i <= m->grid_w; i++) x_edges[i] = (uint16_t)(i * m->canvas_w / m->grid_w);
    for (uint32_t i = 0; i <= m->grid_h; i++) y_edges[i] = (uint16_t)(i * m->canvas_h / m->grid_h);
    memset(pixels, 0, (size_t)m->canvas_w * m->canvas_h * sizeof(uint16_t));
    return DV_OK;
}

static bool read_byte(dv_stream_t *s, uint8_t *out)
{
    if (s->chunk_pos >= s->chunk_len) {
        if (s->next_off >= s->limit) return false;
        size_t want = s->limit - s->next_off;
        if (want > s->chunk_cap) want = s->chunk_cap;
        size_t got = s->read(s->ctx, s->next_off, s->chunk, want);
        if (got == 0) return false;
        if (got > want) got = want;
        s->next_off += (uint32_t)got;
        s->chunk_len = got;
        s->chunk_pos = 0;
    }
    *out = s->chunk[s->chunk_pos++];
    return true;
}

static int read_varint(dv_stream_t *s, uint32_t *out)
{
    uint32_t v = 0;
    for (uint32_t shift = 0; shift <= 21; shift += 7) {
        uint8_t b;
        if (!read_byte(s, &b)) return DV_ERR_EOF;
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return DV_OK;
        }
    }
    return DV_ERR_DATA;
}

int dv_stream_next_frame(dv_stream_t *s)
{
    if (!s || !s->pixels) return DV_ERR_DATA;
    uint8_t h[4] = {0};
    int nb = s->m.count_u32 ? 4 : 2;
    for (int i = 0; i < nb; i++) {
        if (!read_byte(s, &h[i])) return DV_ERR_EOF;
    }
    uint32_t count = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    uint32_t cells = (uint32_t)s->m.grid_w * s->m.grid_h;
    uint32_t idx = 0;
    uint16_t dx0 = 0xFFFF, dy0 = 0xFFFF, dx1 = 0, dy1 = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t packed;
        int err = read_varint(s, &packed);
        if (err != DV_OK) return err;
        uint32_t delta = packed >> 1;
        if (i == 0) {
            idx = delta;
        } else {
            if (delta > cells) return DV_ERR_DATA;   /* also guards overflow */
            idx += delta;
        }
        if (idx >= cells) return DV_ERR_DATA;

        uint16_t px = 0;
        if (!(packed & 1)) {
            uint8_t lo, hi;
            if (!read_byte(s, &lo) || !read_byte(s, &hi)) return DV_ERR_EOF;
            uint16_t c = (uint16_t)(lo | (hi << 8));
            px = s->swap ? (uint16_t)((c >> 8) | (c << 8)) : c;
        }

        uint16_t gx = (uint16_t)(idx % s->m.grid_w), gy = (uint16_t)(idx / s->m.grid_w);
        uint16_t x0 = s->x_edges[gx], x1 = s->x_edges[gx + 1];
        uint16_t y0 = s->y_edges[gy], y1 = s->y_edges[gy + 1];
        if (x0 >= x1 || y0 >= y1) continue;
        for (uint16_t y = y0; y < y1; y++) {
            uint16_t *row = s->pixels + (size_t)y * s->m.canvas_w;
            for (uint16_t x = x0; x < x1; x++) row[x] = px;
        }
        if (x0 < dx0) dx0 = x0;
        if (y0 < dy0) dy0 = y0;
        if (x1 > dx1) dx1 = x1;
        if (y1 > dy1) dy1 = y1;
    }

    if (dx1) {
        s->dirty_x0 = dx0; s->dirty_y0 = dy0; s->dirty_x1 = dx1; s->dirty_y1 = dy1;
    } else {
        s->dirty_x0 = s->dirty_y0 = s->dirty_x1 = s->dirty_y1 = 0;
    }
    s->frame++;
    return DV_OK;
}
