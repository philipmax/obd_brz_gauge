#pragma once
/*
 * video_player.h — Video playback engine
 *
 * Two modes:
 *   - Fullscreen exclusive (boot, showroom): block decode direct to display
 *   - Theme composite: whole-frame decode to PSRAM, rendered via video_view
 *
 * Ring buffer architecture:
 *   Read task (core0, prio 3) → PSRAM ring (1.25MB) → Decode task (core1, prio 5)
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Playback modes */
typedef enum {
    VIDEO_MODE_FULLSCREEN = 0,  /* Exclusive fullscreen (pause LVGL) */
    VIDEO_MODE_COMPOSITE,       /* Composite with LVGL (via video_view) */
} video_mode_t;

/* Player state */
typedef enum {
    VIDEO_STATE_STOPPED = 0,
    VIDEO_STATE_LOADING,
    VIDEO_STATE_PLAYING,
    VIDEO_STATE_PAUSED,
    VIDEO_STATE_ERROR,
} video_state_t;

/* Video source */
typedef enum {
    VIDEO_SRC_FLASH = 0,        /* From bootmedia partition */
    VIDEO_SRC_SD,               /* From SD card */
} video_source_t;

/* Player configuration */
typedef struct {
    video_mode_t mode;
    video_source_t source;
    const char *path;           /* SD path, or NULL for flash */
    uint8_t target_fps;         /* 24 or 30 */
    bool loop;
    lv_obj_t *video_view;       /* For composite mode only */
} video_config_t;

/* Player statistics */
typedef struct {
    uint32_t frames_decoded;
    uint32_t frames_dropped;
    uint32_t buffer_underruns;
    uint32_t avg_decode_time_ms;
    uint32_t current_fps;
    uint32_t ring_used_bytes;
    uint32_t ring_capacity_bytes;
} video_stats_t;

/**
 * Initialize video player.
 */
esp_err_t video_player_init(void);

/**
 * Start playback.
 */
esp_err_t video_player_start(const video_config_t *config);

/**
 * Stop playback and free resources.
 */
esp_err_t video_player_stop(void);

/**
 * Pause/resume.
 */
esp_err_t video_player_pause(void);
esp_err_t video_player_resume(void);

/**
 * Get current state.
 */
video_state_t video_player_get_state(void);

/**
 * Get statistics.
 */
void video_player_get_stats(video_stats_t *stats);

/**
 * Seek to frame (for composite mode).
 */
esp_err_t video_player_seek(uint32_t frame_idx);

#ifdef __cplusplus
}
#endif
