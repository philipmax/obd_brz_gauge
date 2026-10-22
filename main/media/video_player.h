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

/* Longest theme media path is /sdcard/OBD/THEMES/<64-char-key>/<64-char-key>.obv. */
#define VIDEO_PLAYER_PATH_MAX 160u

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
    VIDEO_SRC_FLASH = 0,        /* Retained for source compatibility; rejected */
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
    bool value_driven;         /* OBV1 composite: decode requested position, no clock */
    double position;           /* Initial normalized position, 0..1 */
} video_config_t;

/**
 * Start playback.
 */
esp_err_t video_player_start(const video_config_t *config);

/**
 * Stop playback and free resources.
 */
esp_err_t video_player_stop(void);

/**
 * Pause/resume (LVGL task, or while holding lvgl_mux, like start/stop).
 * Pending composite frames remain owned by the session during pause and are
 * handed to the view after resume. Fullscreen refresh stays
 * paused until stop. Condition waits are event-driven: paused decoding and
 * a full reader ring do not wake on a periodic timeout. Frame deadlines and panel
 * transfer timeouts remain timed. Stop wakes both workers.
 */
esp_err_t video_player_pause(void);
esp_err_t video_player_resume(void);
/* Value-driven composite only; LVGL task / lvgl_mux. Finite 0..1, clamped. */
esp_err_t video_player_set_position(double position);

/**
 * LVGL runtime handoff, called with lvgl_mux held before lv_timer_handler().
 * Workers only wake the boot-lifetime LVGL task; no per-session UI timer.
 */
void video_player_ui_step(void);

/**
 * Get current state.
 */
video_state_t video_player_get_state(void);

/**
 * Stop playback and free the decoder handle, ring buffer and frame buffers
 * (entering OTA / theme switch). The next start re-allocates.
 */
void video_player_release_all(void);

#ifdef __cplusplus
}
#endif
