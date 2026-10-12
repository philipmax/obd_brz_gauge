#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obv_ctx obv_ctx_t;

/**
 * @brief Open OBV1 video file
 * @param path File path
 * @param out_ctx Output context
 * @return ESP_OK on success
 */
esp_err_t obv_open(const char *path, obv_ctx_t **out_ctx);

/**
 * @brief Close OBV1 video file
 * @param ctx Context to close
 */
void obv_close(obv_ctx_t *ctx);

/**
 * @brief Read a single frame
 * @param ctx Context
 * @param frame_num Frame number (0-based)
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @param out_size Output: actual frame size
 * @return ESP_OK on success
 */
esp_err_t obv_read_frame(obv_ctx_t *ctx, uint32_t frame_num, uint8_t *buffer, size_t buffer_size, size_t *out_size);

/**
 * @brief Get video width
 */
uint16_t obv_get_width(obv_ctx_t *ctx);

/**
 * @brief Get video height
 */
uint16_t obv_get_height(obv_ctx_t *ctx);

/**
 * @brief Get frame count
 */
uint16_t obv_get_frame_count(obv_ctx_t *ctx);

/**
 * @brief Get FPS
 */
uint8_t obv_get_fps(obv_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
