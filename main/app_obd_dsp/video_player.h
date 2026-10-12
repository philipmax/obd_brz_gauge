#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VIDEO_PLAYER_IDLE,
    VIDEO_PLAYER_LOADING,
    VIDEO_PLAYER_PLAYING,
    VIDEO_PLAYER_PAUSED,
    VIDEO_PLAYER_STOPPED
} video_player_state_t;

typedef struct {
    void (*on_frame_ready)(uint8_t *frame_data, size_t size);
    void (*on_playback_complete)(void);
    void (*on_error)(esp_err_t error);
} video_player_callbacks_t;

/**
 * @brief Initialize video player
 * @return ESP_OK on success
 */
esp_err_t video_player_init(void);

/**
 * @brief Deinitialize video player
 * @return ESP_OK on success
 */
esp_err_t video_player_deinit(void);

/**
 * @brief Open video file
 * @param path Path to OBV file
 * @return ESP_OK on success
 */
esp_err_t video_player_open(const char *path);

/**
 * @brief Start/resume playback
 * @return ESP_OK on success
 */
esp_err_t video_player_play(void);

/**
 * @brief Pause playback
 * @return ESP_OK on success
 */
esp_err_t video_player_pause(void);

/**
 * @brief Stop playback and close file
 * @return ESP_OK on success
 */
esp_err_t video_player_stop(void);

/**
 * @brief Set player callbacks
 * @param callbacks Callback structure
 */
void video_player_set_callbacks(video_player_callbacks_t *callbacks);

/**
 * @brief Get current player state
 * @return Current state
 */
video_player_state_t video_player_get_state(void);

#ifdef __cplusplus
}
#endif
