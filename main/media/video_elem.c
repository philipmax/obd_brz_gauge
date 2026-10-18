/*
 * video_elem.c — theme compose-mode video element (see video_elem.h).
 *
 * The element is a video_view. Its state hangs off a DELETE callback
 * registered with LV_EVENT_PREPROCESS, so it runs before video_view frees its
 * own state (stopping the player clears the view).
 *
 * video_player has one session, so one element at a time is the owner
 * (s_owner, the element that wants to play: the newest one created or
 * un-paused). A 100 ms lv_timer drives the owner: it starts playback once
 * no fullscreen session holds the panel, mirrors the paused flag, and on a
 * decode error stops and hides the element so the poster image underneath
 * shows. Elements that lose ownership are stopped and show the poster too.
 *
 * All calls from the LVGL task; release_all also from a task that does not
 * hold lvgl_mux.
 */

#include "media/video_elem.h"

#include <stdio.h>
#include <string.h>

#include "bsp_obd_dsp/sd_card.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media/video_player.h"
#include "media/video_player_internal.h"
#include "media/video_view.h"

static const char *TAG = "video_elem";

extern SemaphoreHandle_t lvgl_mux;   /* app_main.c */

#define VE_PATH_MAX   VIDEO_PLAYER_PATH_MAX
#define VE_THEME_MAX  65    /* storage theme key: up to 64 chars plus NUL */
#define VE_POLL_MS    100

typedef struct {
    char theme[VE_THEME_MAX];
    char path[VE_PATH_MAX];   /* "" = no video for the current key */
    bool paused;              /* rule `visible` */
    bool failed;              /* decode / start error, cleared by set_key */
    bool hid;                 /* we set LV_OBJ_FLAG_HIDDEN */
} video_elem_t;

static lv_obj_t   *s_owner;
static lv_timer_t *s_poll;

static void elem_cb(lv_event_t *e);

static video_elem_t *elem_state(lv_obj_t *obj)
{
    return obj ? (video_elem_t *)lv_obj_get_event_user_data(obj, elem_cb) : NULL;
}

/* SD_DIR_THEMES/<theme>/<key>.obv, then SD_DIR_VIDEO/<key>.obv. */
static bool resolve(const char *theme, const char *key, char *out, size_t cap)
{
    if (!key || !key[0] || strchr(key, '/') || !sd_card_is_mounted()) return false;
    int n;
    if (theme && theme[0]) {
        n = snprintf(out, cap, SD_DIR_THEMES "/%s/%s.obv", theme, key);
        if (n > 0 && (size_t)n < cap && sd_card_exists(out)) return true;
    }
    n = snprintf(out, cap, SD_DIR_VIDEO "/%s.obv", key);
    if (n > 0 && (size_t)n < cap && sd_card_exists(out)) return true;
    out[0] = '\0';
    return false;
}

static void drop_owner(void)
{
    s_owner = NULL;
    if (s_poll) {
        lv_timer_del(s_poll);
        s_poll = NULL;
    }
}

static void elem_fail(lv_obj_t *obj, video_elem_t *st)
{
    st->failed = true;
    st->hid = true;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (s_owner == obj) drop_owner();
}

static void elem_tick(void)
{
    lv_obj_t *obj = s_owner;
    video_elem_t *st = elem_state(obj);
    if (!st || st->failed || !st->path[0]) {
        drop_owner();
        return;
    }
    if (video_player_current_view() == obj) {
        video_state_t vs = video_player_get_state();
        if (vs == VIDEO_STATE_ERROR || vs == VIDEO_STATE_STOPPED) {
            ESP_LOGW(TAG, "%s: playback failed, showing the poster", st->path);
            video_player_stop();
            elem_fail(obj, st);
        } else if (st->paused && vs != VIDEO_STATE_PAUSED) {
            video_player_pause();
        } else if (!st->paused && vs == VIDEO_STATE_PAUSED) {
            video_player_resume();
        }
        return;
    }
    /* Do not start an off-screen theme video while its page is being
     * prebuilt during boot. The first timer tick after the page becomes
     * active will start it normally. */
    if (st->paused || video_player_fullscreen_active() || lv_obj_get_screen(obj) != lv_scr_act()) return;
    video_config_t cfg = {
        .mode = VIDEO_MODE_COMPOSITE,
        .source = VIDEO_SRC_SD,
        .path = st->path,
        .target_fps = 0,
        .loop = true,
        .video_view = obj,
    };
    esp_err_t err = video_player_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: start failed: %s", st->path, esp_err_to_name(err));
        elem_fail(obj, st);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    elem_tick();
}

/* obj becomes the element that plays (the previous owner is stopped when
 * the new session starts). */
static void elem_want(lv_obj_t *obj)
{
    s_owner = obj;
    if (!s_poll) s_poll = lv_timer_create(poll_cb, VE_POLL_MS, NULL);
    elem_tick();
}

static void elem_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    lv_obj_t *obj = lv_event_get_target(e);
    /* PREPROCESS: video_view's state is still alive for the player's clear */
    if (video_player_current_view() == obj) video_player_stop();
    if (s_owner == obj) drop_owner();
    heap_caps_free(lv_event_get_user_data(e));
}

lv_obj_t *video_elem_create(lv_obj_t *parent, const char *theme_key, const char *key,
                            lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    if (!parent) return NULL;
    char path[VE_PATH_MAX];
    if (!resolve(theme_key, key, path, sizeof(path))) {
        ESP_LOGD(TAG, "no video for '%s' (theme '%s')", key ? key : "", theme_key ? theme_key : "");
        return NULL;
    }
    video_elem_t *st = heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!st) return NULL;
    lv_obj_t *obj = video_view_create(parent);
    if (!obj) {
        heap_caps_free(st);
        return NULL;
    }
    if (theme_key && strlen(theme_key) < sizeof(st->theme)) strcpy(st->theme, theme_key);
    strlcpy(st->path, path, sizeof(st->path));
    lv_obj_add_event_cb(obj, elem_cb, LV_EVENT_DELETE | LV_EVENT_PREPROCESS, st);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    ESP_LOGI(TAG, "%s at %d,%d %dx%d", path, (int)x, (int)y, (int)w, (int)h);
    elem_want(obj);
    return obj;
}

void video_elem_set_paused(lv_obj_t *obj, bool paused)
{
    video_elem_t *st = elem_state(obj);
    if (!st || st->paused == paused) return;   /* rules re-apply the same value */
    st->paused = paused;
    if (paused) {
        if (video_player_current_view() == obj) video_player_pause();
    } else {
        elem_want(obj);
    }
}

bool video_elem_set_key(lv_obj_t *obj, const char *key)
{
    video_elem_t *st = elem_state(obj);
    if (!st) return false;
    char path[VE_PATH_MAX];
    bool found = resolve(st->theme, key, path, sizeof(path));
    if (found && !st->failed && strcmp(path, st->path) == 0) return true;
    if (video_player_current_view() == obj) video_player_stop();
    strlcpy(st->path, path, sizeof(st->path));   /* "" when not found: poster only */
    st->failed = false;
    if (st->hid) {
        st->hid = false;
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (!found) {
        if (s_owner == obj) drop_owner();
        return false;
    }
    if (!st->paused) elem_want(obj);
    return true;
}

void video_elem_release_all(void)
{
    bool take = lvgl_mux && xSemaphoreGetMutexHolder(lvgl_mux) != xTaskGetCurrentTaskHandle();
    if (take) xSemaphoreTake(lvgl_mux, portMAX_DELAY);
    drop_owner();
    if (video_player_current_view()) video_player_stop();   /* composite = a video_elem */
    if (take) xSemaphoreGive(lvgl_mux);
}
