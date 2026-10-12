#pragma once
/*
 * obv1_parser.h — OBV1 video container parser
 *
 * OBV1 format:
 *   - 64-byte header (magic, version, width, height, fps, frame_count, crc32)
 *   - Frame index table (offset + size per frame)
 *   - Baseline MJPEG frames (concatenated)
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBV1_MAGIC 0x3142564F  /* 'OBV1' */
#define OBV1_VERSION 1

/* OBV1 header (64 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;           /* 'OBV1' */
    uint32_t version;         /* Format version = 1 */
    uint16_t width;
    uint16_t height;
    uint8_t fps;              /* Target framerate */
    uint8_t subsampling;      /* JPEG subsampling (0x11=4:4:4, 0x21=4:2:2, 0x22=4:2:0) */
    uint16_t reserved;
    uint32_t frame_count;
    uint32_t index_offset;    /* Offset to frame index table */
    uint32_t data_offset;     /* Offset to first frame */
    uint32_t max_frame_size;  /* Largest frame in bytes */
    uint32_t crc32;           /* CRC32 of all data (header + index + frames) */
    uint8_t flags;            /* Bit 0: BLOCK_SAFE (safe for 4:2:0 block decode) */
    uint8_t padding[23];
} obv1_header_t;

/* Frame index entry */
typedef struct __attribute__((packed)) {
    uint32_t offset;          /* Offset from data_offset */
    uint32_t size;            /* Frame size in bytes */
} obv1_frame_entry_t;

/* OBV1 context */
typedef struct {
    obv1_header_t header;
    obv1_frame_entry_t *index;
    const uint8_t *data_base; /* Base pointer for frame data */
    bool owns_index;          /* Whether index was allocated */
} obv1_context_t;

/**
 * Parse OBV1 header and validate.
 * Returns ESP_OK if valid OBV1 file.
 */
esp_err_t obv1_parse_header(const uint8_t *data, size_t size, obv1_header_t *header);

/**
 * Open OBV1 context (loads index table).
 * data must remain valid for lifetime of context.
 */
esp_err_t obv1_open(const uint8_t *data, size_t size, obv1_context_t *ctx);

/**
 * Close context and free resources.
 */
void obv1_close(obv1_context_t *ctx);

/**
 * Get frame data pointer and size.
 * Returns ESP_OK if frame_idx valid.
 */
esp_err_t obv1_get_frame(const obv1_context_t *ctx, uint32_t frame_idx,
                         const uint8_t **frame_data, uint32_t *frame_size);

/**
 * Validate CRC32 of entire file.
 */
esp_err_t obv1_validate_crc(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif
