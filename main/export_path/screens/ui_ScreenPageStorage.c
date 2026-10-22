// Storage Page (opened by the SD CARD button on the settings page)
//  - SD state (polled every second while shown) and free / total space.
//  - SCAN:   process /OBD/IMPORT now (sd_import_scan; also runs at boot / card insert).
//  - EJECT:  flush and unmount before pulling the card (sd_card_eject).
//  - EXPORT: copy every installed vehicle profile to /OBD/EXPORT (vp_manager_export).
//  - THEME:  pick a theme of the SD library /OBD/THEMES with < >, APPLY selects it
//            from SD and restarts (ui_ext_theme_activate).
//  SCAN / EXPORT / the space query / the theme list run on a short-lived worker
//  task so the LVGL task never blocks on SD I/O (esp_vfs_fat_info may walk the
//  whole FAT, theme_library_list opens every theme.bin).
//  Buttons are disabled while a background storage job (storage_service) runs.
//  Swipe down/left/right returns to the settings page.

#include "../ui.h"
#include <stdio.h>
#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/sd_import.h"
#include "app_obd_dsp/storage_service.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/theme_library.h"
#include "export_path/ui_ext.h"
#include "media/video_elem.h"
#include "media/video_player.h"
#include "vehicle/vp_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_storage";

typedef enum {
    ST_JOB_STAT = 0,   // refresh the space figures only
    ST_JOB_SCAN,
    ST_JOB_EXPORT,
} st_job_t;

static lv_obj_t *s_lbl_state = NULL;
static lv_obj_t *s_lbl_space = NULL;
static lv_obj_t *s_lbl_msg   = NULL;
static lv_obj_t *s_btn_scan   = NULL;
static lv_obj_t *s_btn_eject  = NULL;
static lv_obj_t *s_btn_export = NULL;
static lv_obj_t *s_btn_prev   = NULL;
static lv_obj_t *s_btn_next   = NULL;
static lv_obj_t *s_btn_apply  = NULL;
static lv_obj_t *s_lbl_theme  = NULL;
static lv_timer_t *s_timer = NULL;

// SD theme library, PSRAM: [0, THEME_MAX) shown, [THEME_MAX, 2*THEME_MAX) worker scratch.
#define THEME_MAX 16
static theme_lib_entry_t *s_themes = NULL;
static int s_theme_n = 0;
static int s_theme_sel = 0;
static volatile int s_theme_scan_n = -1;   // worker result, -1 = list unavailable (busy)
static char s_theme_shown[sizeof(((theme_lib_entry_t *)0)->key)];
static char s_theme_status_shown[72];      // last ui_ext_theme_status() put on the message line

// Worker -> LVGL handoff (single writer each way; polled by the 1 s timer).
static volatile bool      s_job_busy = false;
static volatile bool      s_job_done = false;
static volatile st_job_t  s_job_kind = ST_JOB_STAT;
static volatile esp_err_t s_job_err = ESP_OK;
static volatile int       s_job_count = 0;      // SCAN: files imported, EXPORT: profiles written
static volatile int       s_job_failed = 0;     // SCAN: files rejected
static volatile uint64_t  s_total_bytes = 0;
static volatile uint64_t  s_free_bytes = 0;
static sd_state_t s_last_state = SD_STATE_DISABLED;

static void storage_job_task(void *arg)
{
    st_job_t job = (st_job_t)(intptr_t)arg;
    esp_err_t err = ESP_OK;
    int count = 0, failed = 0;

    if (job == ST_JOB_SCAN) {
        err = sd_import_scan();     // ESP_ERR_INVALID_STATE: busy / unmounted / OTA
        if (err == ESP_OK) sd_import_last_summary(&count, &failed);
    } else if (job == ST_JOB_EXPORT) {
        vpm_entry_t *list = heap_caps_malloc(16 * sizeof(vpm_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!list) {
            err = ESP_ERR_NO_MEM;
        } else if (!storage_job_lock(pdMS_TO_TICKS(2000))) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            int n = vp_manager_list(list, 16);
            for (int i = 0; i < n; i++) {
                if (list[i].builtin) continue;
                esp_err_t e = vp_manager_export(list[i].id);
                if (e == ESP_OK) count++;
                else err = e;
            }
            storage_job_unlock();
        }
        heap_caps_free(list);
    }

    // Every job refreshes the theme list (SCAN may have moved a theme.bin into it).
    s_theme_scan_n = (s_themes && sd_card_is_mounted())
                     ? theme_library_list(s_themes + THEME_MAX, THEME_MAX) : 0;

    if (sd_card_is_mounted()) {
        s_total_bytes = sd_card_get_total_bytes();
        s_free_bytes = sd_card_get_free_bytes();
    } else {
        s_total_bytes = 0;
        s_free_bytes = 0;
    }

    s_job_err = err;
    s_job_count = count;
    s_job_failed = failed;
    s_job_kind = job;
    s_job_done = true;
    s_job_busy = false;
    vTaskDelete(NULL);
}

static bool storage_job_start(st_job_t job)
{
    if (s_job_busy) return false;
    if (s_themes == NULL) {
        s_themes = heap_caps_calloc(2 * THEME_MAX, sizeof(theme_lib_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_job_busy = true;
    if (xTaskCreate(storage_job_task, "ui_sd_job", 8192, (void *)(intptr_t)job, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        s_job_busy = false;
        return false;
    }
    return true;
}

static void st_set_msg(const char *text, bool ok)
{
    lv_label_set_text(s_lbl_msg, text);
    lv_obj_set_style_text_color(s_lbl_msg, ok ? lv_color_hex(0x00CC66) : lv_color_hex(0xFF4040), LV_PART_MAIN);
}

static void st_set_btn_enabled(lv_obj_t *btn, bool en)
{
    if (en) lv_obj_clear_state(btn, LV_STATE_DISABLED);
    else    lv_obj_add_state(btn, LV_STATE_DISABLED);
}

// Integer GB with one decimal, no float formatting.
static void fmt_gb(char *buf, size_t len, uint64_t bytes)
{
    uint64_t tenths = (bytes * 10) >> 30;
    snprintf(buf, len, "%u.%u", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
}

static void theme_row_update(bool enabled)
{
    if (s_theme_n == 0) {
        lv_label_set_text(s_lbl_theme, !sd_card_is_mounted() ? "-" : s_job_busy ? "..." : "No SD themes");
    } else {
        const theme_lib_entry_t *t = &s_themes[s_theme_sel];
        lv_label_set_text_fmt(s_lbl_theme, "%s%s", t->name[0] ? t->name : t->key,
                              t->active ? " (current)" : "");
    }
    bool many = enabled && s_theme_n > 1;
    st_set_btn_enabled(s_btn_prev, many);
    st_set_btn_enabled(s_btn_next, many);
    st_set_btn_enabled(s_btn_apply, enabled && s_theme_n > 0);
}

// Worker finished: take its list, keep the selection on the same key when possible.
static void theme_list_take(void)
{
    int n = s_theme_scan_n;
    if (n < 0 || s_themes == NULL) return;   // busy: keep the previous list
    if (n > THEME_MAX) n = THEME_MAX;
    memcpy(s_themes, s_themes + THEME_MAX, (size_t)n * sizeof(theme_lib_entry_t));
    s_theme_n = n;
    s_theme_sel = 0;
    for (int i = 0; i < n; i++) {
        if (s_theme_shown[0] ? strcmp(s_themes[i].key, s_theme_shown) == 0 : s_themes[i].active) {
            s_theme_sel = i;
            break;
        }
    }
    if (n > 0) strlcpy(s_theme_shown, s_themes[s_theme_sel].key, sizeof(s_theme_shown));
}

static void storage_page_refresh(void)
{
    sd_state_t st = sd_card_get_state();
    const char *txt;
    lv_color_t col;
    switch (st) {
    case SD_STATE_MOUNTED:  txt = "SD READY";      col = lv_color_hex(0x00CC66); break;
    case SD_STATE_ABSENT:   txt = "NO CARD";       col = lv_color_hex(0x888888); break;
    case SD_STATE_ERROR:    txt = "CARD ERROR";    col = lv_color_hex(0xFF4040); break;
    case SD_STATE_EJECTED:  txt = "EJECTED";       col = lv_color_hex(0xFFCC00); break;
    default:                txt = "NO SD SLOT";    col = lv_color_hex(0x888888); break;
    }
    lv_label_set_text(s_lbl_state, txt);
    lv_obj_set_style_text_color(s_lbl_state, col, LV_PART_MAIN);

    bool mounted = (st == SD_STATE_MOUNTED);
    if (mounted && st != s_last_state) storage_job_start(ST_JOB_STAT);   // card (re)appeared
    s_last_state = st;

    if (mounted && s_total_bytes > 0) {
        char f[16], t[16], line[48];
        fmt_gb(f, sizeof(f), s_free_bytes);
        fmt_gb(t, sizeof(t), s_total_bytes);
        snprintf(line, sizeof(line), "%s / %s GB FREE", f, t);
        lv_label_set_text(s_lbl_space, line);
    } else if (st == SD_STATE_EJECTED) {
        lv_label_set_text(s_lbl_space, "Safe to remove");
    } else if (st == SD_STATE_ERROR) {
        lv_label_set_text(s_lbl_space, "Use FAT32");
    } else {
        lv_label_set_text(s_lbl_space, mounted ? "..." : "");
    }

    bool idle = !s_job_busy && !storage_service_busy();
    st_set_btn_enabled(s_btn_scan, mounted && idle);
    st_set_btn_enabled(s_btn_export, mounted && idle);
    st_set_btn_enabled(s_btn_eject, mounted && idle);

    if (!mounted) s_theme_n = 0;
    if (s_job_done) theme_list_take();
    bool theme_ok = true;
    const char *theme_status = ui_ext_theme_status(&theme_ok);
    bool switching = theme_status[0] && theme_ok;   // writing / restarting (errors allow a retry)
    theme_row_update(mounted && idle && !switching && theme_library_get_state(NULL) == THEME_LIB_IDLE);

    if (strcmp(theme_status, s_theme_status_shown) != 0) {
        strlcpy(s_theme_status_shown, theme_status, sizeof(s_theme_status_shown));
        if (theme_status[0]) st_set_msg(theme_status, theme_ok);
    }
    if (s_job_done) {
        s_job_done = false;
        char line[48];
        if (s_job_kind == ST_JOB_SCAN) {
            if (s_job_err == ESP_ERR_INVALID_STATE) {
                st_set_msg("Busy, try again", false);
            } else if (s_job_err != ESP_OK) {
                st_set_msg(esp_err_to_name(s_job_err), false);
            } else if (s_job_count == 0 && s_job_failed == 0) {
                st_set_msg("Nothing to import", true);
            } else {
                snprintf(line, sizeof(line), "%d imported, %d failed\nsee IMPORT.LOG", s_job_count, s_job_failed);
                st_set_msg(line, s_job_failed == 0);
            }
        } else if (s_job_kind == ST_JOB_EXPORT) {
            if (s_job_err == ESP_OK && s_job_count == 0) {
                st_set_msg("No installed profiles", true);
            } else if (s_job_err == ESP_OK) {
                snprintf(line, sizeof(line), "%d exported to /OBD/EXPORT", s_job_count);
                st_set_msg(line, true);
            } else if (s_job_err == ESP_ERR_INVALID_STATE) {
                st_set_msg("Busy, try again", false);
            } else {
                st_set_msg(esp_err_to_name(s_job_err), false);
            }
        }
    }
}

static void storage_timer_cb(lv_timer_t *t)
{
    (void)t;
    storage_page_refresh();
}

static void on_scan_click(lv_event_t *e)
{
    (void)e;
    if (storage_job_start(ST_JOB_SCAN)) st_set_msg("Scanning...", true);
    storage_page_refresh();
}

static void on_export_click(lv_event_t *e)
{
    (void)e;
    if (storage_job_start(ST_JOB_EXPORT)) st_set_msg("Exporting...", true);
    storage_page_refresh();
}

static void on_theme_step(lv_event_t *e)
{
    if (s_theme_n < 2) return;
    int step = (lv_event_get_target(e) == s_btn_prev) ? -1 : 1;
    s_theme_sel = (s_theme_sel + step + s_theme_n) % s_theme_n;
    strlcpy(s_theme_shown, s_themes[s_theme_sel].key, sizeof(s_theme_shown));
    storage_page_refresh();
}

static void on_theme_apply(lv_event_t *e)
{
    (void)e;
    if (s_theme_n == 0 || s_job_busy || storage_service_busy()) return;
    esp_err_t err = ui_ext_theme_activate(s_themes[s_theme_sel].key);
    if (err == ESP_ERR_INVALID_STATE) st_set_msg("Busy, try again", false);
    else if (err != ESP_OK)           st_set_msg("Not a usable theme.bin", false);
    storage_page_refresh();
}

static void on_eject_click(lv_event_t *e)
{
    (void)e;
    if (s_job_busy || storage_service_busy()) {
        st_set_msg("Busy, try again", false);
        return;
    }
    // Theme videos stream from the card: stop them (posters stay) before unmounting.
    video_elem_release_all();
    video_player_release_all();
    esp_err_t err = sd_card_eject();
    if (err == ESP_OK) st_set_msg("Card ejected", true);
    else               st_set_msg(esp_err_to_name(err), false);
    s_total_bytes = 0;
    s_free_bytes = 0;
    storage_page_refresh();
}

// The page is created once; poll the SD state only while it is on screen.
static void on_screen_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOAD_START) {
        lv_label_set_text(s_lbl_msg, "");
        s_last_state = SD_STATE_DISABLED;   // forces a space query when mounted
        storage_page_refresh();
        if (s_timer == NULL) s_timer = lv_timer_create(storage_timer_cb, 1000, NULL);
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        if (s_timer) {
            lv_timer_del(s_timer);
            s_timer = NULL;
        }
    }
}

static lv_obj_t *make_st_btn(lv_obj_t *parent, const char *txt, lv_coord_t w, lv_coord_t x, lv_coord_t y,
                             lv_color_t bg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, 30);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void ui_ScreenPageStorage_screen_init(void)
{
    ui_ScreenPageStorage = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageStorage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageStorage, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageStorage);
    lv_obj_set_style_bg_opa(ui_ScreenPageStorage, 255, LV_PART_MAIN);

    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageStorage, 10);

    // Black ear image at top (created before the widgets so they draw on top of the notch)
    lv_obj_t *ear = lv_img_create(ui_ScreenPageStorage);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_set_width(ear, LV_SIZE_CONTENT);
    lv_obj_set_height(ear, LV_SIZE_CONTENT);
    lv_obj_set_pos(ear, 0, -142);
    lv_obj_set_align(ear, LV_ALIGN_CENTER);
    lv_obj_add_flag(ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ear, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(ui_ScreenPageStorage);
    lv_label_set_text(title, "STORAGE");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -112);

    s_lbl_state = lv_label_create(ui_ScreenPageStorage);
    lv_obj_set_style_text_font(s_lbl_state, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_align(s_lbl_state, LV_ALIGN_CENTER, 0, -80);

    s_lbl_space = lv_label_create(ui_ScreenPageStorage);
    lv_obj_set_style_text_font(s_lbl_space, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_space, ui_theme_color_lv(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_align(s_lbl_space, LV_ALIGN_CENTER, 0, -54);

    s_btn_scan   = make_st_btn(ui_ScreenPageStorage, "SCAN",   84, -90, -18, lv_color_hex(0x00AA55), on_scan_click);
    s_btn_export = make_st_btn(ui_ScreenPageStorage, "EXPORT", 84,   0, -18, lv_color_hex(0x333333), on_export_click);
    s_btn_eject  = make_st_btn(ui_ScreenPageStorage, "EJECT",  84,  90, -18, lv_color_hex(0xAA7700), on_eject_click);

    // Theme row: < name >, then APPLY
    s_btn_prev = make_st_btn(ui_ScreenPageStorage, LV_SYMBOL_LEFT,  40, -112, 24, lv_color_hex(0x333333), on_theme_step);
    s_btn_next = make_st_btn(ui_ScreenPageStorage, LV_SYMBOL_RIGHT, 40,  112, 24, lv_color_hex(0x333333), on_theme_step);
    lv_obj_set_style_text_font(lv_obj_get_child(s_btn_prev, 0), LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_obj_get_child(s_btn_next, 0), LV_FONT_DEFAULT, LV_PART_MAIN);

    s_lbl_theme = lv_label_create(ui_ScreenPageStorage);
    lv_obj_set_style_text_font(s_lbl_theme, LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_theme, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_long_mode(s_lbl_theme, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_theme, 168);
    lv_obj_set_style_text_align(s_lbl_theme, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_lbl_theme, LV_ALIGN_CENTER, 0, 24);

    s_btn_apply = make_st_btn(ui_ScreenPageStorage, "APPLY THEME", 140, 0, 62, lv_color_hex(0x0066CC), on_theme_apply);

    s_lbl_msg = lv_label_create(ui_ScreenPageStorage);
    lv_label_set_text(s_lbl_msg, "");
    lv_obj_set_style_text_font(s_lbl_msg, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_long_mode(s_lbl_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_msg, 230);
    lv_obj_set_style_text_align(s_lbl_msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_lbl_msg, LV_ALIGN_CENTER, 0, 102);

    lv_obj_t *hint = lv_label_create(ui_ScreenPageStorage);
    lv_label_set_text(hint, "Swipe: back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 150);

    storage_page_refresh();

    lv_obj_move_foreground(ring);
    lv_obj_add_event_cb(ui_ScreenPageStorage, on_screen_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_ScreenPageStorage, ui_event_storage_background, LV_EVENT_GESTURE, NULL);
}
