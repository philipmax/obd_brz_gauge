/*
 * resource_pipeline.c — Theme asset conversion pipeline
 */

#include "resource_pipeline.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>

static const char *TAG = "res_pipeline";

/* Helper: convert RGBA8888 pixel to RGB565 */
static inline uint16_t rgba_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

esp_err_t resource_convert_rgba8888(const uint8_t *rgba_data, uint16_t width, uint16_t height,
                                     converted_asset_t *out) {
    if (!rgba_data || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    /* RGB565A8 planar format: RGB565 plane + A8 plane */
    size_t rgb_size = width * height * 2;  /* RGB565: 2 bytes per pixel */
    size_t alpha_size = width * height;     /* A8: 1 byte per pixel */
    size_t total_size = rgb_size + alpha_size;

    uint8_t *converted = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!converted) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for conversion", total_size);
        return ESP_ERR_NO_MEM;
    }

    uint16_t *rgb_plane = (uint16_t *)converted;
    uint8_t *alpha_plane = converted + rgb_size;

    /* Convert pixel by pixel */
    for (uint32_t i = 0; i < width * height; i++) {
        uint8_t r = rgba_data[i * 4 + 0];
        uint8_t g = rgba_data[i * 4 + 1];
        uint8_t b = rgba_data[i * 4 + 2];
        uint8_t a = rgba_data[i * 4 + 3];

        rgb_plane[i] = rgba_to_rgb565(r, g, b);
        alpha_plane[i] = a;
    }

    /* Create LVGL descriptor */
    memset(&out->dsc, 0, sizeof(out->dsc));
    out->dsc.header.always_zero = 0;
    out->dsc.header.w = width;
    out->dsc.header.h = height;
    out->dsc.header.cf = LV_IMG_CF_RGB565A8;
    out->dsc.data_size = total_size;
    out->dsc.data = converted;

    out->data = converted;
    out->data_size = total_size;
    out->needs_free = true;

    ESP_LOGD(TAG, "Converted %ux%u RGBA8888 to RGB565A8 (%zu bytes)", width, height, total_size);
    return ESP_OK;
}

esp_err_t resource_convert_rgb565(const uint8_t *rgb_data, uint16_t width, uint16_t height,
                                   converted_asset_t *out) {
    if (!rgb_data || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    /* RGB565 big-endian: zero-copy, just create descriptor */
    memset(&out->dsc, 0, sizeof(out->dsc));
    out->dsc.header.always_zero = 0;
    out->dsc.header.w = width;
    out->dsc.header.h = height;
    out->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    out->dsc.data_size = width * height * 2;
    out->dsc.data = rgb_data;

    out->data = (void *)rgb_data;
    out->data_size = width * height * 2;
    out->needs_free = false;  /* Zero-copy */

    return ESP_OK;
}

esp_err_t resource_apply_alpha_bleed(uint8_t *rgb565a8_data, uint16_t width, uint16_t height) {
    if (!rgb565a8_data) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Alpha bleeding: extend color from opaque pixels into transparent neighbors
     * to prevent black edges when rotated */

    uint16_t *rgb_plane = (uint16_t *)rgb565a8_data;
    uint8_t *alpha_plane = rgb565a8_data + (width * height * 2);

    /* Simple 3x3 dilation */
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int idx = y * width + x;

            if (alpha_plane[idx] < 128) {
                /* Transparent pixel, bleed from neighbors */
                uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
                int count = 0;

                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx;
                        int ny = y + dy;
                        int nidx = ny * width + nx;

                        if (alpha_plane[nidx] >= 128) {
                            uint16_t rgb = rgb_plane[nidx];
                            r_sum += (rgb >> 11) & 0x1F;
                            g_sum += (rgb >> 5) & 0x3F;
                            b_sum += rgb & 0x1F;
                            count++;
                        }
                    }
                }

                if (count > 0) {
                    uint8_t r = r_sum / count;
                    uint8_t g = g_sum / count;
                    uint8_t b = b_sum / count;
                    rgb_plane[idx] = (r << 11) | (g << 5) | b;
                }
            }
        }
    }

    return ESP_OK;
}

esp_err_t resource_prescale_pointer(const converted_asset_t *src, float scale,
                                     converted_asset_t *out) {
    if (!src || !out || scale <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t new_width = (uint16_t)(src->dsc.header.w * scale);
    uint16_t new_height = (uint16_t)(src->dsc.header.h * scale);

    /* Simple nearest-neighbor scaling for now */
    /* TODO: implement area-averaged downsampling with pre-multiplied alpha */

    size_t rgb_size = new_width * new_height * 2;
    size_t alpha_size = new_width * new_height;
    size_t total_size = rgb_size + alpha_size;

    uint8_t *scaled = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scaled) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t *src_rgb = (uint16_t *)src->data;
    uint8_t *src_alpha = (uint8_t *)src->data + (src->dsc.header.w * src->dsc.header.h * 2);

    uint16_t *dst_rgb = (uint16_t *)scaled;
    uint8_t *dst_alpha = scaled + rgb_size;

    for (uint16_t y = 0; y < new_height; y++) {
        for (uint16_t x = 0; x < new_width; x++) {
            uint16_t src_x = (uint16_t)(x / scale);
            uint16_t src_y = (uint16_t)(y / scale);

            int src_idx = src_y * src->dsc.header.w + src_x;
            int dst_idx = y * new_width + x;

            dst_rgb[dst_idx] = src_rgb[src_idx];
            dst_alpha[dst_idx] = src_alpha[src_idx];
        }
    }

    memset(&out->dsc, 0, sizeof(out->dsc));
    out->dsc.header.w = new_width;
    out->dsc.header.h = new_height;
    out->dsc.header.cf = LV_IMG_CF_RGB565A8;
    out->dsc.data_size = total_size;
    out->dsc.data = scaled;

    out->data = scaled;
    out->data_size = total_size;
    out->needs_free = true;

    return ESP_OK;
}

esp_err_t resource_prebake_rotation(const converted_asset_t *src, int16_t angle,
                                     converted_asset_t *out) {
    /* TODO: implement rotation pre-baking */
    /* For now, return error - rotation will be done at runtime */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t resource_prebake_recolor(const converted_asset_t *src, lv_color_t color,
                                    converted_asset_t *out) {
    /* TODO: implement recolor pre-baking */
    /* For now, return error - recolor will be done at runtime */
    return ESP_ERR_NOT_SUPPORTED;
}

void resource_free_converted(converted_asset_t *asset) {
    if (!asset) {
        return;
    }

    if (asset->needs_free && asset->data) {
        heap_caps_free(asset->data);
    }

    memset(asset, 0, sizeof(*asset));
}
