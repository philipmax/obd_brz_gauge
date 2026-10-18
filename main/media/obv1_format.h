#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * OBV1 - MJPEG video container (written by tools/make_video.py).
 *
 * All integers little-endian. File layout:
 *
 *   [header 64 B][frame index, 8 B x frame_count][frame data]
 *
 * Header:
 *   off  size  field
 *   0x00  4    magic           "OBV1" (4F 42 56 31)
 *   0x04  2    version         1
 *   0x06  2    header_size     64
 *   0x08  2    width           px, 8..360, multiple of 8
 *   0x0A  2    height          px, 8..360, multiple of 8
 *   0x0C  2    fps             1..60
 *   0x0E  1    subsampling     0 gray, 1 4:4:4, 2 4:2:2, 3 4:2:0 (all frames)
 *   0x0F  1    flags           bit0 BLOCK_SAFE, bit1 LOOP (hint); others 0
 *   0x10  4    frame_count     1..OBV1_MAX_FRAMES
 *   0x14  4    index_offset    = 64
 *   0x18  4    data_offset     = 64 + 8 * frame_count
 *   0x1C  4    data_size       bytes of frame data (file size = data_offset + data_size)
 *   0x20  4    max_frame_size  largest frame, <= OBV1_FRAME_MAX
 *   0x24  4    duration_ms     frame_count * 1000 / fps (informational)
 *   0x28  4    index_crc32     CRC-32 of the frame index
 *   0x2C  4    data_crc32      CRC-32 of the frame data (checked by `verify`,
 *                              not by the player, which streams)
 *   0x30 12    reserved        0
 *   0x3C  4    header_crc32    CRC-32 of bytes 0x00..0x3B
 *
 * Frame index entry i (at index_offset + 8 * i):
 *   0x00  4    offset          relative to data_offset; entries ascending and
 *                              non-overlapping, offset + size <= data_size
 *   0x04  4    size            4..max_frame_size
 *
 * Each frame is one complete baseline JPEG (SOI..EOI), width x height,
 * same subsampling as the header; no restart markers required.
 * CRC-32 is the zlib / IEEE 802.3 one (poly 0xEDB88320, init and xorout
 * 0xFFFFFFFF), i.e. Python zlib.crc32().
 *
 * BLOCK_SAFE: the encoder guarantees width % 8 == 0, height % 8 == 0 and,
 * for 4:2:0, height % 16 == 0 (no half MCU row). The player only uses
 * esp_new_jpeg block (strip) decoding when this flag is set AND the boot
 * self-test passed for that subsampling; otherwise it decodes whole frames.
 *
 * This header is pure C (no ESP-IDF) so test/host/media can build it.
 * -------------------------------------------------------------------------- */

#define OBV1_HEADER_SIZE     64u
#define OBV1_INDEX_ENTRY     8u
#define OBV1_VERSION         1u
#define OBV2_VERSION         2u
#define OBV2_INDEX_ENTRY     16u
#define OBV2_TIMESCALE       1000000u
#define OBV2_RING_MAX        (1280u * 1024u)
#define OBV1_FRAME_MAX       (48u * 1024u)  /* per-frame cap, make_video.py enforces the same */
#define OBV1_MAX_FRAMES      36000u         /* index: v1 <= 288 KB, v2 <= 576 KB */
#define OBV1_MAX_DIM         360u

#define OBV1_FLAG_BLOCK_SAFE 0x01u
#define OBV1_FLAG_LOOP       0x02u

#define OBV1_SUB_GRAY        0u
#define OBV1_SUB_444         1u
#define OBV1_SUB_422         2u
#define OBV1_SUB_420         3u

typedef enum {
    OBV1_OK = 0,
    OBV1_ERR_TRUNCATED = -1,  /* buffer / file shorter than the header says */
    OBV1_ERR_MAGIC = -2,
    OBV1_ERR_VERSION = -3,
    OBV1_ERR_CRC = -4,
    OBV1_ERR_GEOMETRY = -5,   /* width / height / fps / subsampling / offsets */
    OBV1_ERR_FRAME_SIZE = -6, /* a frame (or max_frame_size) above the cap */
    OBV1_ERR_INDEX = -7,      /* entry out of range, overlapping or descending */
} obv1_err_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint8_t  subsampling;
    uint8_t  flags;
    uint32_t frame_count;
    uint32_t index_offset;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t max_frame_size;
    uint32_t duration_ms;
    uint32_t index_crc32;
    uint32_t data_crc32;
    uint16_t version;          /* appended: v1 fixed FPS, v2 per-frame microseconds */
} obv1_info_t;

/* zlib-compatible CRC-32; pass 0 to start, feed the previous result to continue. */
uint32_t obv1_crc32(uint32_t crc, const void *data, size_t len);

/* Parse and validate the 64-byte header. file_size = total file length
 * (0 = unknown, skips the truncation check of the data region); frame_cap =
 * largest acceptable frame (0 = OBV1_FRAME_MAX). */
int obv1_parse_header(const uint8_t *buf, size_t len, uint64_t file_size,
                      uint32_t frame_cap, obv1_info_t *out);

/* Validate the whole index (CRC + every entry). len must be >= obv_index_stride(info) * frame_count. */
int obv1_check_index(const obv1_info_t *info, const uint8_t *index, size_t len);

/* Entry i of a validated index (no bounds check). */
void obv1_index_entry(const uint8_t *index, uint32_t i, uint32_t *offset, uint32_t *size);

/* V2 retains the OBV1 magic/header; version=2, 4:2:0 or 4:2:2 baseline JPEG,
 * fps=0, 0x30=1000000,
 * 0x34..0x3b=0. Index stride=16: offset,size,timestamp_us,duration_us.
 * Timestamps start at 0 and are contiguous, duration >=16666 us (<=60 fps).
 * duration_ms=ceil(sum(duration_us)/1000). Frames remain baseline JPEG.
 * V1 callers keep the original 8-byte accessor. */
uint32_t obv_index_stride(const obv1_info_t *info);
void obv_index_frame(const obv1_info_t *info, const uint8_t *index, uint32_t i,
                     uint32_t *offset, uint32_t *size);
uint32_t obv_frame_time(const uint8_t *index, uint32_t i);
uint32_t obv_frame_duration(const uint8_t *index, uint32_t i);
/* Exact largest one-second compressed window + wrap/headroom; 0 if over budget. */
uint32_t obv_timed_ring_size(const obv1_info_t *info, const uint8_t *index);
uint32_t obv_timed_prebuffer(const obv1_info_t *info, const uint8_t *index);

/* Duration in ms derived from frame_count / fps. */
uint32_t obv1_duration_ms(const obv1_info_t *info);

const char *obv1_strerror(int err);

#ifdef __cplusplus
}
#endif
