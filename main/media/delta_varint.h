#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * delta_varint - legacy boot animation stream (bootmedia_format 1), streamed.
 *
 * Manifest (text, key=value per line, 0xFF / NUL terminated):
 *   canvas_width, canvas_height   output size, 1..512
 *   grid_width, grid_height       cell grid, 1..512; cell edges = i*canvas/grid
 *   fps, frame_count              required
 *   duration_ms                   legacy metadata; playback uses frame_count/fps
 *   binary_size                   optional, stream length in bytes
 *   stream_format                 delta_varint_rgb565_black_v1 (default) | _v2
 *
 * Stream, per frame:
 *   change_count                  u16 LE (v1) or u32 LE (v2)
 *   change_count x {
 *     varint (LEB128, <= 4 bytes) = delta << 1 | is_black
 *       cell index = delta for the first change, previous index + delta after
 *     if !is_black: u16 LE RGB565 colour
 *   }
 *
 * The decoder reads through a caller-owned chunk buffer (16 KB, may be PSRAM)
 * and a read callback, so the stream never has to fit in RAM.
 * Pure C (no ESP-IDF), built by test/host/media.
 * -------------------------------------------------------------------------- */

#define DV_MAX_DIM  512u

typedef enum {
    DV_OK = 0,
    DV_ERR_MANIFEST = -1,
    DV_ERR_EOF = -2,      /* stream ended before frame_count frames */
    DV_ERR_DATA = -3,     /* cell index out of range / bad varint */
} dv_err_t;

typedef struct {
    uint16_t canvas_w;
    uint16_t canvas_h;
    uint16_t grid_w;
    uint16_t grid_h;
    uint16_t fps;
    bool     count_u32;     /* v2 */
    uint32_t frame_count;
    uint32_t duration_ms;
    uint32_t binary_size;   /* 0 = not in manifest */
} dv_manifest_t;

/* Returns bytes copied into dst (0 = end of data / read error). */
typedef size_t (*dv_read_fn)(void *ctx, uint32_t off, uint8_t *dst, size_t len);

typedef struct {
    dv_manifest_t m;
    dv_read_fn read;
    void      *ctx;
    uint8_t   *chunk;       /* caller buffer */
    size_t     chunk_cap;
    size_t     chunk_len;
    size_t     chunk_pos;
    uint32_t   next_off;    /* stream offset of the next refill */
    uint32_t   limit;       /* stream length available */
    uint16_t  *x_edges;     /* grid_w + 1 entries, caller buffer */
    uint16_t  *y_edges;     /* grid_h + 1 entries, caller buffer */
    uint16_t  *pixels;      /* canvas_w * canvas_h, caller buffer */
    bool       swap;        /* store byte-swapped RGB565 (LV_COLOR_16_SWAP) */
    uint32_t   frame;       /* frames decoded so far */
    /* dirty rectangle of the last decoded frame (x1/y1 exclusive; empty when x1 == 0) */
    uint16_t   dirty_x0, dirty_y0, dirty_x1, dirty_y1;
} dv_stream_t;

/* Parse a manifest (len bytes, need not be NUL terminated). */
int dv_parse_manifest(const char *text, size_t len, dv_manifest_t *out);

/* Bind buffers and clear the canvas to black. limit = readable stream bytes
 * (fails when the manifest's binary_size exceeds it). */
int dv_stream_init(dv_stream_t *s, const dv_manifest_t *m, dv_read_fn read, void *ctx,
                   uint32_t limit, uint8_t *chunk, size_t chunk_cap,
                   uint16_t *x_edges, uint16_t *y_edges, uint16_t *pixels, bool swap);

/* Decode the next frame into pixels. */
int dv_stream_next_frame(dv_stream_t *s);

#ifdef __cplusplus
}
#endif
