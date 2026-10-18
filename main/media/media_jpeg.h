#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * media_jpeg - thin wrapper over esp_new_jpeg 1.0.2 (baseline JPEG decoder).
 *
 * Output is always RGB565 big-endian, i.e. the byte order LVGL uses with
 * LV_COLOR_16_SWAP=y and the order the ST77916 expects on the wire.
 *
 * One handle per playback: media_jpeg_open() at start, media_jpeg_close() at
 * stop. Per frame: media_jpeg_begin() (parses the header, required on every
 * frame), then either
 *   - block mode: media_jpeg_next_block() `info.blocks` times; each call
 *     writes one MCU row (8 or 16 lines) of width*2-byte lines, or
 *   - frame mode: media_jpeg_decode_frame() once.
 * Every output pointer must be 16-byte aligned (decoder requirement).
 * Block mode needs width/height multiples of 8, no scaling.
 * -------------------------------------------------------------------------- */

typedef struct media_jpeg media_jpeg_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t mcu_rows;    /* block mode: lines per block call (8 or 16); 0 in frame mode */
    uint32_t block_len;   /* block mode: bytes one call may write (reserve this much) */
    uint32_t blocks;      /* block calls per frame (1 in frame mode) */
} media_jpeg_info_t;

esp_err_t media_jpeg_open(bool block_mode, media_jpeg_t **out);
void      media_jpeg_close(media_jpeg_t *j);

/* Parse the next frame's header. data must stay valid until the frame is done. */
esp_err_t media_jpeg_begin(media_jpeg_t *j, const uint8_t *data, size_t len,
                           media_jpeg_info_t *info);
/* Block mode: decode the next MCU row into out (block_len bytes writable).
 * *lines = picture lines produced by this call (clamped to the picture height). */
esp_err_t media_jpeg_next_block(media_jpeg_t *j, uint8_t *out, uint16_t *lines);
/* Frame mode: decode the whole picture into out (width*height*2 bytes). */
esp_err_t media_jpeg_decode_frame(media_jpeg_t *j, uint8_t *out, size_t out_cap);

/* True when block mode was verified for this OBV1 subsampling (OBV1_SUB_*).
 * Runs that subsampling's self-test on first use (~100 ms, cached). Gray and
 * 4:4:4 have no test image and always report false (frame-mode fallback). */
bool      media_jpeg_block_ok(uint8_t subsampling);
#ifdef __cplusplus
}
#endif
