#include <stdio.h>
#include <string.h>

#include "media/video_elem.h"
#include "media/video_player.h"
#include "media/video_player_internal.h"
#include "media/video_view.h"
#include "freertos/semphr.h"
#include "th_util.h"

SemaphoreHandle_t lvgl_mux;

static lv_obj_t *playing_view;
static const char *fail_path;
static unsigned starts;
static unsigned stops;
static bool value_driven;
static double position;
static bool sd_mounted = true;

bool sd_card_is_mounted(void) { return sd_mounted; }
bool sd_card_exists(const char *path) { return path && strstr(path, ".obv") != NULL; }

lv_obj_t *video_view_create(lv_obj_t *parent) { return lv_obj_create(parent); }
void video_view_set_frame(lv_obj_t *obj, const uint16_t *pixels, uint16_t w, uint16_t h)
{ (void)obj; (void)pixels; (void)w; (void)h; }
void video_view_clear(lv_obj_t *obj) { (void)obj; }

esp_err_t video_player_start(const video_config_t *config)
{
    starts++;
    if (fail_path && strcmp(config->path, fail_path) == 0) return ESP_FAIL;
    playing_view = config->video_view;
    value_driven = config->value_driven;
    position = config->position;
    return ESP_OK;
}
esp_err_t video_player_stop(void)
{
    stops++;
    playing_view = NULL;
    return ESP_OK;
}
esp_err_t video_player_set_position(double p) { position = p; return ESP_OK; }
esp_err_t video_player_pause(void) { return ESP_OK; }
esp_err_t video_player_resume(void) { return ESP_OK; }
video_state_t video_player_get_state(void) { return playing_view ? VIDEO_STATE_PLAYING : VIDEO_STATE_STOPPED; }
lv_obj_t *video_player_current_view(void) { return playing_view; }
uint32_t video_player_session_id(void) { return 0; }
bool video_player_fullscreen_active(void) { return false; }
bool video_player_first_frame_presented(void) { return playing_view != NULL; }
void video_player_release_all(void) { (void)video_player_stop(); }
SemaphoreHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t mutex) { (void)mutex; return NULL; }
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) { (void)mutex; (void)wait; return 1; }
int xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; return 1; }
void *xTaskGetCurrentTaskHandle(void) { return NULL; }

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_disp_flush_ready(drv);
}

static void test_layer_handoff(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *lower_poster = lv_img_create(screen);
    lv_obj_t *lower_box = lv_obj_create(screen);
    lv_obj_t *upper_poster = lv_img_create(screen);
    lv_obj_t *upper_box = lv_obj_create(screen);
    lv_obj_t *lower = video_elem_create(lower_box, "test", "asuka", 0, 0, 100, 100);
    lv_obj_t *upper = video_elem_create(upper_box, "test", "shinji", 0, 0, 100, 100);

    TH_CHECK(lower != NULL && upper != NULL);
    TH_CHECK(playing_view == upper);

    /* Match theme_page: hide both the static poster and the video wrapper,
     * then pause the video element so the lower layer takes decoder ownership. */
    lv_obj_add_flag(upper_poster, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(upper_box, LV_OBJ_FLAG_HIDDEN);
    video_elem_set_paused(upper, true);
    TH_CHECK(playing_view == lower);
    TH_CHECK(lv_obj_has_flag(lower_poster, LV_OBJ_FLAG_HIDDEN) == false);
    TH_CHECK(lv_obj_has_flag(upper_poster, LV_OBJ_FLAG_HIDDEN));
    TH_CHECK(lv_obj_has_flag(upper_box, LV_OBJ_FLAG_HIDDEN));

    lv_obj_clear_flag(upper_poster, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(upper_box, LV_OBJ_FLAG_HIDDEN);
    video_elem_set_paused(upper, false);
    TH_CHECK(playing_view == upper);

    video_elem_set_opa(upper, LV_OPA_TRANSP);
    TH_CHECK(playing_view == lower);
    video_elem_set_opa(upper, LV_OPA_COVER);
    TH_CHECK(playing_view == upper);

    fail_path = "/OBD/THEMES/test/shinji.obv";
    video_elem_set_opa(upper, LV_OPA_TRANSP);
    video_elem_set_opa(upper, LV_OPA_COVER);
    TH_CHECK(lv_obj_has_flag(upper, LV_OBJ_FLAG_HIDDEN));
    TH_CHECK(playing_view == lower);
    fail_path = NULL;

    lv_obj_del(upper_box);
    TH_CHECK(playing_view == lower);
    lv_obj_del(lower_box);
    TH_CHECK(playing_view == NULL);
    TH_CHECK(starts >= 6 && stops >= 5);
    video_elem_release_all();
}

static void test_value_position_handoff(void)
{
    lv_obj_t *box = lv_obj_create(lv_scr_act());
    unsigned before = starts;
    lv_obj_t *video = video_elem_create_value(box, "test", "rpm", 0, 0, 100, 100);
    TH_CHECK(video && value_driven && position == 0 && starts == before + 1);
    video_elem_set_position(video, 0.75);
    TH_CHECK(position == 0.75);
    video_elem_set_opa(video, LV_OPA_TRANSP);
    video_elem_set_position(video, 0.25);
    video_elem_set_opa(video, LV_OPA_COVER);
    TH_CHECK(playing_view == video && value_driven && position == 0.25);
    TH_CHECK(video_elem_set_key(video, "replacement"));
    TH_CHECK(value_driven && position == 0.25);
    video_elem_set_position(video, 1);
    TH_CHECK(position == 1);
    lv_obj_del(box);
    video_elem_release_all();
}

static void test_invalid_key_and_missing_sd(void)
{
    lv_obj_t *box = lv_obj_create(lv_scr_act());
    lv_obj_t *video = video_elem_create(box, "test", "original", 0, 0, 100, 100);
    TH_CHECK(video && playing_view == video);

    TH_CHECK(!video_elem_set_key(video, NULL));
    TH_CHECK(playing_view == NULL);
    TH_CHECK(video_elem_set_key(video, "original"));
    TH_CHECK(!video_elem_set_key(video, "bad/path"));
    TH_CHECK(playing_view == NULL);
    TH_CHECK(video_elem_set_key(video, "original"));

    sd_mounted = false;
    TH_CHECK(!video_elem_set_key(video, "other"));
    TH_CHECK(playing_view == NULL);
    sd_mounted = true;
    TH_CHECK(video_elem_set_key(video, "original"));
    TH_CHECK(playing_view == video);
    lv_obj_del(box);
    video_elem_release_all();
}

int main(void)
{
    lv_init();
    static lv_color_t pixels[360 * 8];
    static lv_disp_draw_buf_t draw;
    static lv_disp_drv_t driver;
    lv_disp_draw_buf_init(&draw, pixels, NULL, 360 * 8);
    lv_disp_drv_init(&driver);
    driver.hor_res = driver.ver_res = 360;
    driver.draw_buf = &draw;
    driver.flush_cb = flush;
    lv_disp_t *display = lv_disp_drv_register(&driver);
    test_layer_handoff(); test_value_position_handoff(); test_invalid_key_and_missing_sd();
    lv_disp_remove(display);
    return th_report("theme_video_elem");
}
