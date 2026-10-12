/*
 * obv1_parser.c — OBV1 video container parser
 */

#include "obv1_parser.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "obv1_parser";

esp_err_t obv1_parse_header(const uint8_t *data, size_t size, obv1_header_t *header) {
    if (!data || size < sizeof(obv1_header_t) || !header) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(header, data, sizeof(obv1_header_t));

    /* Validate magic */
    if (header->magic != OBV1_MAGIC) {
        ESP_LOGE(TAG, "Invalid magic: 0x%08X (expected 0x%08X)", header->magic, OBV1_MAGIC);
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate version */
    if (header->version != OBV1_VERSION) {
        ESP_LOGE(TAG, "Unsupported version: %u", header->version);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Validate dimensions */
    if (header->width == 0 || header->height == 0 || header->width > 1024 || header->height > 1024) {
        ESP_LOGE(TAG, "Invalid dimensions: %ux%u", header->width, header->height);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Validate frame count */
    if (header->frame_count == 0) {
        ESP_LOGE(TAG, "Zero frames");
        return ESP_ERR_INVALID_SIZE;
    }

    /* Validate offsets */
    if (header->index_offset < sizeof(obv1_header_t) ||
        header->data_offset < header->index_offset ||
        header->index_offset + (header->frame_count * sizeof(obv1_frame_entry_t)) > size) {
        ESP_LOGE(TAG, "Invalid offsets");
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate max frame size */
    if (header->max_frame_size == 0 || header->max_frame_size > (1024 * 1024)) {
        ESP_LOGE(TAG, "Invalid max_frame_size: %u", header->max_frame_size);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "OBV1: %ux%u, %u fps, %u frames, max %u KB/frame",
             header->width, header->height, header->fps, header->frame_count,
             header->max_frame_size / 1024);

    return ESP_OK;
}

esp_err_t obv1_open(const uint8_t *data, size_t size, obv1_context_t *ctx) {
    if (!data || !ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Parse and validate header */
    esp_err_t err = obv1_parse_header(data, size, &ctx->header);
    if (err != ESP_OK) {
        return err;
    }

    /* Allocate and load index table */
    size_t index_size = ctx->header.frame_count * sizeof(obv1_frame_entry_t);
    ctx->index = heap_caps_malloc(index_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->index) {
        ESP_LOGE(TAG, "Failed to allocate index table (%zu bytes)", index_size);
        return ESP_ERR_NO_MEM;
    }

    memcpy(ctx->index, data + ctx->header.index_offset, index_size);
    ctx->owns_index = true;

    /* Set data base pointer */
    ctx->data_base = data + ctx->header.data_offset;

    ESP_LOGI(TAG, "OBV1 opened: %u frames", ctx->header.frame_count);
    return ESP_OK;
}

void obv1_close(obv1_context_t *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->owns_index && ctx->index) {
        heap_caps_free(ctx->index);
    }

    memset(ctx, 0, sizeof(*ctx));
}

esp_err_t obv1_get_frame(const obv1_context_t *ctx, uint32_t frame_idx,
                         const uint8_t **frame_data, uint32_t *frame_size) {
    if (!ctx || !ctx->index || !frame_data || !frame_size) {
        return ESP_ERR_INVALID_ARG;
    }

    if (frame_idx >= ctx->header.frame_count) {
        return ESP_ERR_INVALID_ARG;
    }

    const obv1_frame_entry_t *entry = &ctx->index[frame_idx];

    *frame_data = ctx->data_base + entry->offset;
    *frame_size = entry->size;

    return ESP_OK;
}

esp_err_t obv1_validate_crc(const uint8_t *data, size_t size) {
    if (!data || size < sizeof(obv1_header_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    obv1_header_t header;
    memcpy(&header, data, sizeof(header));

    if (header.magic != OBV1_MAGIC) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Calculate CRC over data after header */
    uint32_t calc_crc = esp_crc32_le(0, data + sizeof(obv1_header_t),
                                      size - sizeof(obv1_header_t));

    if (calc_crc != header.crc32) {
        ESP_LOGE(TAG, "CRC mismatch: got 0x%08X, expected 0x%08X", calc_crc, header.crc32);
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}
