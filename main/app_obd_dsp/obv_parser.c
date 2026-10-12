/*
 * OBV1 Video Container Parser
 * Format: 64B header + frame index + MJPEG frames
 */

#include "obv_parser.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "obv";

#define OBV_MAGIC 0x4F425631  // "OBV1"
#define MAX_FRAME_SIZE (48 * 1024)  // 48KB per frame

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint8_t fps;
    uint8_t reserved[53];
} __attribute__((packed)) obv_header_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} __attribute__((packed)) obv_frame_entry_t;

struct obv_ctx {
    FILE *file;
    obv_header_t header;
    obv_frame_entry_t *frame_index;
    uint32_t current_frame;
};

esp_err_t obv_open(const char *path, obv_ctx_t **out_ctx) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    obv_ctx_t *ctx = calloc(1, sizeof(obv_ctx_t));
    if (!ctx) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    ctx->file = f;

    // Read header
    if (fread(&ctx->header, sizeof(obv_header_t), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to read header");
        fclose(f);
        free(ctx);
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->header.magic != OBV_MAGIC) {
        ESP_LOGE(TAG, "Invalid magic: 0x%08X", ctx->header.magic);
        fclose(f);
        free(ctx);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "OBV: %dx%d, %d frames, %d fps",
             ctx->header.width, ctx->header.height,
             ctx->header.frame_count, ctx->header.fps);

    // Read frame index
    size_t index_size = ctx->header.frame_count * sizeof(obv_frame_entry_t);
    ctx->frame_index = malloc(index_size);
    if (!ctx->frame_index) {
        fclose(f);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    if (fread(ctx->frame_index, index_size, 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to read frame index");
        free(ctx->frame_index);
        fclose(f);
        free(ctx);
        return ESP_ERR_INVALID_ARG;
    }

    // Validate frame sizes
    for (int i = 0; i < ctx->header.frame_count; i++) {
        if (ctx->frame_index[i].size > MAX_FRAME_SIZE) {
            ESP_LOGE(TAG, "Frame %d too large: %u bytes", i, ctx->frame_index[i].size);
            free(ctx->frame_index);
            fclose(f);
            free(ctx);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    *out_ctx = ctx;
    return ESP_OK;
}

void obv_close(obv_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->file) {
        fclose(ctx->file);
    }
    if (ctx->frame_index) {
        free(ctx->frame_index);
    }
    free(ctx);
}

esp_err_t obv_read_frame(obv_ctx_t *ctx, uint32_t frame_num, uint8_t *buffer, size_t buffer_size, size_t *out_size) {
    if (!ctx || !buffer || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    if (frame_num >= ctx->header.frame_count) {
        return ESP_ERR_INVALID_ARG;
    }

    obv_frame_entry_t *entry = &ctx->frame_index[frame_num];
    if (entry->size > buffer_size) {
        ESP_LOGE(TAG, "Buffer too small: need %u, have %u", entry->size, buffer_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (fseek(ctx->file, entry->offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Seek failed for frame %u", frame_num);
        return ESP_ERR_INVALID_STATE;
    }

    size_t read = fread(buffer, 1, entry->size, ctx->file);
    if (read != entry->size) {
        ESP_LOGE(TAG, "Read failed: got %u, expected %u", read, entry->size);
        return ESP_ERR_INVALID_STATE;
    }

    *out_size = entry->size;
    return ESP_OK;
}

uint16_t obv_get_width(obv_ctx_t *ctx) {
    return ctx ? ctx->header.width : 0;
}

uint16_t obv_get_height(obv_ctx_t *ctx) {
    return ctx ? ctx->header.height : 0;
}

uint16_t obv_get_frame_count(obv_ctx_t *ctx) {
    return ctx ? ctx->header.frame_count : 0;
}

uint8_t obv_get_fps(obv_ctx_t *ctx) {
    return ctx ? ctx->header.fps : 0;
}
