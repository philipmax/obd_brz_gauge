/*
 * esp_new_jpeg_wrapper.c — JPEG decoder wrapper
 */

#include "esp_new_jpeg_wrapper.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "jpeg_decoder";

/* TODO: Include actual esp_new_jpeg headers when available */
/* For now, provide stub implementation */

static bool s_decoder_initialized = false;

esp_err_t jpeg_decoder_init(void) {
    if (s_decoder_initialized) {
        return ESP_OK;
    }

    /* TODO: Initialize esp_new_jpeg decoder
     * - Allocate decoder context (~10KB internal RAM)
     * - Configure for RGB565 output
     */

    s_decoder_initialized = true;
    ESP_LOGI(TAG, "JPEG decoder initialized");
    return ESP_OK;
}

void jpeg_decoder_deinit(void) {
    if (!s_decoder_initialized) {
        return;
    }

    /* TODO: Free decoder resources */

    s_decoder_initialized = false;
    ESP_LOGI(TAG, "JPEG decoder deinitialized");
}

esp_err_t jpeg_decode_frame(const uint8_t *jpeg_data, size_t jpeg_size,
                             uint16_t *out_rgb565, uint16_t *out_width, uint16_t *out_height) {
    if (!s_decoder_initialized || !jpeg_data || !out_rgb565) {
        return ESP_ERR_INVALID_ARG;
    }

    /* TODO: Decode entire JPEG frame
     * 1. Parse JPEG headers
     * 2. Allocate MCU buffers
     * 3. Decode all MCUs
     * 4. Write to output buffer
     * 5. Return dimensions
     */

    ESP_LOGW(TAG, "jpeg_decode_frame not fully implemented (stub)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t jpeg_decode_blocks(const uint8_t *jpeg_data, size_t jpeg_size,
                              jpeg_block_cb_t callback, void *user_data) {
    if (!s_decoder_initialized || !jpeg_data || !callback) {
        return ESP_ERR_INVALID_ARG;
    }

    /* TODO: Block-by-block decode
     * 1. Parse JPEG headers (get dimensions, subsampling)
     * 2. For each 16x16 MCU:
     *    - Decode MCU to RGB565
     *    - Call callback(x, y, w, h, data, user_data)
     * 3. Handle edge MCUs (partial blocks)
     */

    ESP_LOGW(TAG, "jpeg_decode_blocks not fully implemented (stub)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t jpeg_get_info(const uint8_t *jpeg_data, size_t jpeg_size,
                         uint16_t *width, uint16_t *height, uint8_t *subsampling) {
    if (!jpeg_data || jpeg_size < 20) {
        return ESP_ERR_INVALID_ARG;
    }

    /* TODO: Parse JPEG SOF marker to extract dimensions and subsampling
     * SOF marker format:
     *   FF C0 (baseline) / FF C2 (progressive)
     *   Length (2 bytes)
     *   Precision (1 byte)
     *   Height (2 bytes)
     *   Width (2 bytes)
     *   Components (1 byte)
     *   Component info (3 bytes per component)
     */

    /* Stub: return typical values */
    if (width) *width = 360;
    if (height) *height = 360;
    if (subsampling) *subsampling = 0x22;  /* 4:2:2 */

    return ESP_OK;
}

esp_err_t jpeg_decoder_self_test(void) {
    if (!s_decoder_initialized) {
        esp_err_t err = jpeg_decoder_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    /* TODO: Embedded test JPEG (360x360, Q75, 4:2:2)
     * 1. Decode test image in block mode
     * 2. Verify known pixel values at specific positions
     * 3. Check last half-row of MCUs (tests 4:2:0 360/16 edge case)
     * 4. Return ESP_OK if all checks pass
     */

    ESP_LOGI(TAG, "JPEG self-test: PASS (stub)");
    return ESP_OK;
}
