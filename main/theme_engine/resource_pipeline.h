#pragma once
/*
 * resource_pipeline.h — Theme asset conversion pipeline
 *
 * Converts editor assets to LVGL-native formats:
 *   - rgb565: big-endian, zero-copy
 *   - rgba8888: convert to RGB565A8 (planar, 3B/px) at page build time
 *   - Crop to visible area
 *   - Corner masking for round display
 *   - Alpha bleeding for rotation
 */

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Converted asset descriptor */
typedef struct {
    lv_img_dsc_t dsc;             /* LVGL image descriptor */
    void *data;                   /* Converted data in PSRAM */
    uint32_t data_size;           /* Size in bytes */
    bool needs_free;              /* Whether data was allocated */
} converted_asset_t;

/**
 * Convert rgba8888 to RGB565A8 planar format.
 * Output is allocated in PSRAM.
 * Returns ESP_OK on success.
 */
esp_err_t resource_convert_rgba8888(const uint8_t *rgba_data, uint16_t width, uint16_t height,
                                     converted_asset_t *out);

/**
 * Convert rgb565 big-endian (zero-copy, just create descriptor).
 */
esp_err_t resource_convert_rgb565(const uint8_t *rgb_data, uint16_t width, uint16_t height,
                                   converted_asset_t *out);

/**
 * Apply alpha bleeding to prevent black edges on rotation.
 * Modifies data in-place.
 */
esp_err_t resource_apply_alpha_bleed(uint8_t *rgb565a8_data, uint16_t width, uint16_t height);

/**
 * Pre-scale pointer image (area-averaged downsampling with pre-multiplied alpha).
 */
esp_err_t resource_prescale_pointer(const converted_asset_t *src, float scale,
                                     converted_asset_t *out);

/**
 * Pre-bake rotation for static images.
 */
esp_err_t resource_prebake_rotation(const converted_asset_t *src, int16_t angle,
                                     converted_asset_t *out);

/**
 * Pre-bake recolor.
 */
esp_err_t resource_prebake_recolor(const converted_asset_t *src, lv_color_t color,
                                    converted_asset_t *out);

/**
 * Free converted asset.
 */
void resource_free_converted(converted_asset_t *asset);

#ifdef __cplusplus
}
#endif
