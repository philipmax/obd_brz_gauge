#pragma once
/*
 * esp_new_jpeg_wrapper.h — Wrapper for esp_new_jpeg decoder
 *
 * Single global decoder handle (~10KB internal RAM).
 * Supports whole-frame and block-based decoding.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decoder context (opaque) */
typedef struct jpeg_decoder_ctx jpeg_decoder_ctx_t;

/**
 * Initialize JPEG decoder (global singleton).
 * Allocates ~10KB internal RAM for decoder state.
 */
esp_err_t jpeg_decoder_init(void);

/**
 * Deinitialize decoder and free resources.
 */
void jpeg_decoder_deinit(void);

/**
 * Decode entire frame to RGB565.
 * Output buffer must be width * height * 2 bytes.
 */
esp_err_t jpeg_decode_frame(const uint8_t *jpeg_data, size_t jpeg_size,
                             uint16_t *out_rgb565, uint16_t *out_width, uint16_t *out_height);

/**
 * Decode frame block-by-block (for direct display without PSRAM framebuffer).
 * Callback is called for each 16x16 MCU block.
 */
typedef void (*jpeg_block_cb_t)(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 const uint16_t *rgb565_data, void *user_data);

esp_err_t jpeg_decode_blocks(const uint8_t *jpeg_data, size_t jpeg_size,
                              jpeg_block_cb_t callback, void *user_data);

/**
 * Get decoder info without full decode.
 */
esp_err_t jpeg_get_info(const uint8_t *jpeg_data, size_t jpeg_size,
                         uint16_t *width, uint16_t *height, uint8_t *subsampling);

/**
 * Self-test: decode embedded test image and verify output.
 * Called on boot to check esp_new_jpeg block mode correctness.
 */
esp_err_t jpeg_decoder_self_test(void);

#ifdef __cplusplus
}
#endif
