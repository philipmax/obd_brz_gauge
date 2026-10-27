/*
 * video_elem.c — theme compose-mode video element (see video_elem.h).
 *
 * The element is a video_view. Its state hangs off a DELETE callback
 * registered with LV_EVENT_PREPROCESS, so it runs before video_view frees its
 * own state (stopping the player clears the view).
 *
 * video_player has one session/decoder, so the topmost eligible element is
 * the owner. A 100 ms lv_timer selects by page-layer order and starts playback
 * once no fullscreen session holds the panel. Decode errors hide that element
 * so its poster shows and allow the next eligible layer to take ownership.
 *
 * All calls from the LVGL task; release_all also from a task that does not
 * hold lvgl_mux.
 */

#include "media/video_elem.h"
#include "media/video_elem_order.h"

#include <math.h>
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

typedef struct video_elem_state {
    char theme[VE_THEME_MAX];
    char path[VE_PATH_MAX];   /* "" = no video for the current key */
    bool paused;              /* rule `visible` */
    bool failed;              /* decode / start error, cleared by set_key */
    bool hid;                 /* we set LV_OBJ_FLAG_HIDDEN */
    struct video_elem_state *next;
    lv_obj_t *obj;
    bool value_driven;
    double position;
} video_elem_t;

static lv_obj_t   *s_owner;
static lv_timer_t *s_poll;
static video_elem_t *s_elements;

static void elem_cb(lv_event_t *e);
static void poll_cb(lv_timer_t *t);
static void elem_tick(void);

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

static bool obj_visible(lv_obj_t *obj)
{
    if (!obj || lv_obj_get_screen(obj) != lv_scr_act()) return false;
    for (lv_obj_t *it = obj; it; it = lv_obj_get_parent(it)) {
        if (lv_obj_has_flag(it, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

static bool obj_opaque(lv_obj_t *obj)
{
    for (lv_obj_t *it = obj; it; it = lv_obj_get_parent(it)) {
        if (lv_obj_get_style_opa(it, LV_PART_MAIN) == LV_OPA_TRANSP) return false;
        if (it == obj && lv_obj_get_style_img_opa(it, LV_PART_MAIN) == LV_OPA_TRANSP) return false;
    }
    return true;
}

static bool elem_eligible(const video_elem_t *st)
{
    return st && !st->failed && !st->paused && st->path[0] && obj_visible(st->obj) &&
           obj_opaque(st->obj);
}

static video_elem_t *top_visible_element(void)
{
    video_elem_t *best = NULL;
    video_elem_layer_choice_t choice = {0};
    for (video_elem_t *st = s_elements; st; st = st->next) {
        if (video_elem_layer_consider(&choice, elem_eligible(st),
                                      video_elem_layer_order(st->obj))) {
            best = st;
        }
    }
    return best;
}

static void choose_owner(void)
{
    video_elem_t *want = top_visible_element();
    if (!((want && want->obj == s_owner) || (!want && !s_owner))) {
        if (video_player_current_view()) video_player_stop();
        s_owner = want ? want->obj : NULL;
    }
    /* Prebuilt pages are not active yet. Keep polling while elements exist so
     * activating their screen can select an owner without an event/rule. */
    if (s_elements && !s_poll) s_poll = lv_timer_create(poll_cb, VE_POLL_MS, NULL);
    if (!s_elements && s_poll) {
        lv_timer_del(s_poll);
        s_poll = NULL;
    }
}

static void elem_fail(lv_obj_t *obj, video_elem_t *st)
{
    st->failed = true;
    st->hid = true;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (s_owner == obj) s_owner = NULL;
}

static void elem_tick(void)
{
    /* A failed start must not leave the next eligible layer waiting for the
     * timer. The guard also protects against a malformed object list. */
    for (unsigned attempt = 0; attempt <= 128; attempt++) {
        choose_owner();
        lv_obj_t *obj = s_owner;
        video_elem_t *st = elem_state(obj);
        if (!st || st->failed || !st->path[0]) {
            return;
        }
        if (video_player_current_view() == obj) {
            video_state_t vs = video_player_get_state();
            if (vs == VIDEO_STATE_ERROR || vs == VIDEO_STATE_STOPPED) {
                ESP_LOGW(TAG, "%s: playback failed, showing the poster", st->path);
                video_player_stop();
                elem_fail(obj, st);
                continue;
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
            /* Cap theme playback at 24 fps: every frame is a full-screen blit
             * on the same QSPI bus as LVGL's UI flush, so native 30/60 fps
             * competes with the 16 ms needle redraw while OBD data refreshes. */
            .target_fps = 24,
            .loop = true,
            .video_view = obj,
            .value_driven = st->value_driven,
            .position = st->position,
        };
        esp_err_t err = video_player_start(&cfg);
        if (err == ESP_OK) return;
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
    (void)obj;
    if (!s_poll) s_poll = lv_timer_create(poll_cb, VE_POLL_MS, NULL);
    choose_owner();
    elem_tick();
}

static void elem_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    lv_obj_t *obj = lv_event_get_target(e);
    /* PREPROCESS: video_view's state is still alive for the player's clear */
    if (video_player_current_view() == obj) video_player_stop();
    video_elem_t *st = lv_event_get_user_data(e);
    bool was_owner = s_owner == obj;
    if (was_owner) s_owner = NULL;
    video_elem_t **link = &s_elements;
    while (*link && *link != st) link = &(*link)->next;
    if (*link == st) *link = st->next;
    heap_caps_free(st);
    choose_owner();
}

static lv_obj_t *elem_create(lv_obj_t *parent, const char *theme_key, const char *key,
                            lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                            bool value_driven)
{
    if (!parent) return NULL;
    char path[VE_PATH_MAX];
    if (!resolve(theme_key, key, path, sizeof(path))) {
        ESP_LOGD(TAG, "no video for '%s' (theme '%s')", key ? key : "", theme_key ? theme_key : "");
        return NULL;
    }
    lv_obj_t *obj = video_view_create(parent);
    if (!obj) return NULL;
    video_elem_t *st = heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!st) {
        lv_obj_del(obj);
        return NULL;
    }
    st->obj = obj;
    st->value_driven = value_driven;
    st->next = s_elements;
    s_elements = st;
    if (theme_key && strlen(theme_key) < sizeof(st->theme)) strcpy(st->theme, theme_key);
    strlcpy(st->path, path, sizeof(st->path));
    lv_obj_add_event_cb(obj, elem_cb, LV_EVENT_DELETE | LV_EVENT_PREPROCESS, st);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    ESP_LOGI(TAG, "%s at %d,%d %dx%d", path, (int)x, (int)y, (int)w, (int)h);
    elem_want(obj);
    return obj;
}

lv_obj_t *video_elem_create(lv_obj_t *parent, const char *theme_key, const char *key,
                            lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    return elem_create(parent, theme_key, key, x, y, w, h, false);
}

lv_obj_t *video_elem_create_value(lv_obj_t *parent, const char *theme_key, const char *key,
                                  lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    return elem_create(parent, theme_key, key, x, y, w, h, true);
}

void video_elem_set_position(lv_obj_t *obj, double position)
{
    video_elem_t *st = elem_state(obj);
    if (!st || !isfinite(position)) return;
    if (!st->value_driven && video_player_current_view() == obj) video_player_stop();
    st->value_driven = true;
    st->position = fmax(0.0, fmin(1.0, position));
    if (video_player_current_view() == obj) video_player_set_position(st->position);
    else elem_tick();
}

void video_elem_set_paused(lv_obj_t *obj, bool paused)
{
    video_elem_t *st = elem_state(obj);
    if (!st || st->paused == paused) return;   /* rules re-apply the same value */
    st->paused = paused;
    if (paused) {
        if (video_player_current_view() == obj) video_player_pause();
        choose_owner();
        elem_tick();
    } else {
        elem_want(obj);
    }
}

void video_elem_set_opa(lv_obj_t *obj, lv_opa_t opa)
{
    video_elem_t *st = elem_state(obj);
    if (!st) return;
    lv_obj_set_style_img_opa(obj, opa, LV_PART_MAIN);
    /* Releasing an owner at 0% and reclaiming it when it becomes visible must
     * happen synchronously with the rule update, not on a later frame. */
    elem_tick();
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
        if (s_owner == obj) {
            s_owner = NULL;
            choose_owner();
        }
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
