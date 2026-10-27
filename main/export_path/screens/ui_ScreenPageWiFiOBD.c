// WiFi OBD page: pick the ELM327 adapter's hotspot from a WiFi scan.
// Layout mirrors the BLE scan page: saved network (with delete) + nearby list.
// The AP password is fixed (12345678); the TCP address defaults to
// 192.168.0.10:35000 and is stored in NVS.
//
// Selecting a network persists conn_type=WiFi + SSID. If the WiFi transport is
// already active it rejoins at runtime; switching from BLE still reboots
// because the BLE stack has to be released (see obd_link.c).

#include "../ui.h"
#include "bsp_obd_dsp/elm327_wifi_client.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/obd_link.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG_WIFI_UI = "wifi_obd_ui";

static lv_obj_t *s_list = NULL;
static lv_obj_t *s_label_status = NULL;
static lv_obj_t *s_spinner = NULL;
static lv_obj_t *s_saved_panel = NULL;
static lv_obj_t *s_label_saved_hdr = NULL;
static lv_obj_t *s_saved_name_lbl = NULL;
static bool s_scanning = false;
static bool s_restarting = false;

static char s_ap_ssid[WIFI_SCAN_MAX_DEVICES][33];
static int  s_ap_count = 0;
static lv_timer_t *s_restart_timer = NULL;

static void on_network_selected(lv_event_t *e);

extern SemaphoreHandle_t lvgl_mux;
static inline bool lvgl_lock_ui(int timeout_ms) {
    return xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static inline void lvgl_unlock_ui(void) {
    xSemaphoreGive(lvgl_mux);
}

static void restart_timer_cb(lv_timer_t *t)
{
    (void)t;
    esp_restart();
}

static void schedule_restart(void)
{
    if (s_restarting) return;
    s_restarting = true;
    lv_label_set_text(s_label_status, "Saved. Restarting...");
    if (s_restart_timer) lv_timer_del(s_restart_timer);
    s_restart_timer = lv_timer_create(restart_timer_cb, 600, NULL);
}

// Back button: same action as the left/right swipe (stop the scan first).
static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    elm327_wifi_scan_stop();
    s_scanning = false;
    _ui_screen_change(&ui_ScreenPageConnSelect, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                      &ui_ScreenPageConnSelect_screen_init);
}

// Scan callback (scan worker task; LVGL updates must be locked).
static void scan_result_cb(const wifi_scan_result_t *ap, int total_count)
{
    if (!s_list) return;
    if (!lvgl_lock_ui(100)) return;

    uint32_t child_cnt = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(s_list, i);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl && strcmp(lv_label_get_text(lbl), ap->ssid) == 0) {
            lvgl_unlock_ui();
            return;   // already listed
        }
    }
    if (s_ap_count >= WIFI_SCAN_MAX_DEVICES) {
        lvgl_unlock_ui();
        return;
    }

    lv_obj_t *btn = lv_list_add_btn(s_list, LV_SYMBOL_WIFI, ap->ssid);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(btn, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_network_selected, LV_EVENT_CLICKED, NULL);

    strlcpy(s_ap_ssid[s_ap_count], ap->ssid, sizeof(s_ap_ssid[0]));
    s_ap_count++;

    lv_label_set_text_fmt(s_label_status, "Found %d networks", total_count);
    lvgl_unlock_ui();
}

// A hotspot was tapped: save it and reboot into the WiFi OBD transport.
static void on_network_selected(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint32_t idx = lv_obj_get_index(btn);
    if (idx >= (uint32_t)s_ap_count) return;

    const char *ssid = s_ap_ssid[idx];
    ESP_LOGI(TAG_WIFI_UI, "Selected WiFi OBD hotspot: %s", ssid);

    nvs_user_cfg_t base;
    nvs_cfg_snapshot(&base);
    nvs_user_cfg_t cfg = base;
    cfg.conn_type = 1;
    strlcpy(cfg.wifi_obd_ssid, ssid, sizeof(cfg.wifi_obd_ssid));
    nvs_cfg_update(&base, &cfg);

    if (s_saved_name_lbl) lv_label_set_text(s_saved_name_lbl, ssid);
    if (s_saved_panel)    lv_obj_clear_flag(s_saved_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_label_saved_hdr) lv_obj_clear_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);
    if (s_spinner) lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    elm327_wifi_scan_stop();
    s_scanning = false;
    if (obd_link_is_wifi()) {
        /* Already the WiFi transport: resume/rejoin at runtime, no reboot. */
        lv_label_set_text(s_label_status, "Connecting...");
        if (s_spinner) lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
        obd_link_switch_to_wifi(ssid);
    } else {
        /* BLE -> WiFi: the BLE stack must be released, so apply at boot. */
        schedule_restart();
    }
}

// Delete the saved network: drop the WiFi link now; the next selection starts
// the matching transport at runtime (no reboot).
static void on_saved_network_delete(lv_event_t *e)
{
    (void)e;
    nvs_user_cfg_t base;
    nvs_cfg_snapshot(&base);
    nvs_user_cfg_t cfg = base;
    cfg.conn_type = 0;
    cfg.wifi_obd_ssid[0] = '\0';
    nvs_cfg_update(&base, &cfg);

    if (s_saved_panel)    lv_obj_add_flag(s_saved_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_label_saved_hdr) lv_obj_add_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);

    /* Drop the STA/TCP link immediately; the user can then pick BLE or another
     * hotspot on the spot (runtime switch, no reboot needed). */
    obd_link_stop_wifi();
    lv_label_set_text(s_label_status, "Saved network removed");
}

static void start_scan(void)
{
    if (s_scanning) return;
    s_scanning = true;

    if (s_list) lv_obj_clean(s_list);
    if (s_label_status) lv_label_set_text(s_label_status, "Scanning...");
    if (s_spinner) lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    s_ap_count = 0;
    elm327_wifi_scan_start(10, scan_result_cb);
}

static void on_screen_visibility(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        start_scan();
    } else {
        elm327_wifi_scan_stop();
        s_scanning = false;
    }
}

static void on_gesture(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        elm327_wifi_scan_stop();
        s_scanning = false;
        lv_indev_wait_release(lv_indev_get_act());
        _ui_screen_change(&ui_ScreenPageConnSelect, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                          &ui_ScreenPageConnSelect_screen_init);
    }
}

void ui_ScreenPageWiFiOBD_screen_init(void)
{
    nvs_user_cfg_t saved_cfg = nvs_cfg_read();
    bool has_saved = saved_cfg.wifi_obd_ssid[0] != '\0';

    ui_ScreenPageWiFiOBD = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageWiFiOBD, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageWiFiOBD, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_helpers_style_screen_bg(ui_ScreenPageWiFiOBD);
    lv_obj_set_style_bg_opa(ui_ScreenPageWiFiOBD, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ScreenPageWiFiOBD, 0, LV_PART_MAIN);

    lv_obj_t *spinner_ring = ui_helpers_create_ring(ui_ScreenPageWiFiOBD, 10);

    lv_obj_t *label_title = lv_label_create(ui_ScreenPageWiFiOBD);
    lv_label_set_text(label_title, "WIFI OBD");
    lv_obj_set_style_text_font(label_title, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 30);

    // Back button (same action as the swipe; keeps the round display usable)
    lv_obj_t *btn_back = lv_btn_create(ui_ScreenPageWiFiOBD);
    lv_obj_set_size(btn_back, 36, 36);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 56, 50);
    lv_obj_set_style_radius(btn_back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_back, 220, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_back, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_back, 0, LV_PART_MAIN);
    lv_obj_t *back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(btn_back, on_back_clicked, LV_EVENT_CLICKED, NULL);

    s_spinner = lv_spinner_create(ui_ScreenPageWiFiOBD, 1000, 60);
    lv_obj_set_size(s_spinner, 24, 24);
    lv_obj_align(s_spinner, LV_ALIGN_TOP_MID, 72, 20);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN);

    s_label_status = lv_label_create(ui_ScreenPageWiFiOBD);
    lv_label_set_text(s_label_status,
                      (obd_link_is_wifi() && elm327_wifi_is_connected())
                          ? "Connected" : "Select adapter hotspot");
    lv_obj_set_style_text_font(s_label_status, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_status, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(s_label_status, LV_ALIGN_TOP_MID, 0, 50);

    // ==== SAVED NETWORK SECTION ====
    s_label_saved_hdr = lv_label_create(ui_ScreenPageWiFiOBD);
    lv_label_set_text(s_label_saved_hdr, "SAVED NETWORK");
    lv_obj_set_style_text_font(s_label_saved_hdr, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_saved_hdr, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(s_label_saved_hdr, LV_ALIGN_TOP_MID, 0, 72);
    if (!has_saved) lv_obj_add_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);

    s_saved_panel = lv_obj_create(ui_ScreenPageWiFiOBD);
    lv_obj_remove_style_all(s_saved_panel);
    lv_obj_set_size(s_saved_panel, 264, 32);
    lv_obj_align(s_saved_panel, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(s_saved_panel, lv_color_hex(0x222222), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_saved_panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_saved_panel, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_saved_panel, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_saved_panel, LV_OBJ_FLAG_SCROLLABLE);
    if (!has_saved) lv_obj_add_flag(s_saved_panel, LV_OBJ_FLAG_HIDDEN);

    s_saved_name_lbl = lv_label_create(s_saved_panel);
    lv_label_set_text(s_saved_name_lbl, has_saved ? saved_cfg.wifi_obd_ssid : "");
    lv_obj_set_style_text_font(s_saved_name_lbl, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_saved_name_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(s_saved_name_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *del_btn = lv_btn_create(s_saved_panel);
    lv_obj_set_style_clip_corner(del_btn, true, 0);
    lv_obj_set_size(del_btn, 30, 24);
    lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xBB2222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(del_btn, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(del_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(del_btn, 2, LV_PART_MAIN);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_label_set_text(del_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(del_lbl);
    lv_obj_add_event_cb(del_btn, on_saved_network_delete, LV_EVENT_CLICKED, NULL);

    lv_obj_t *divider = lv_obj_create(ui_ScreenPageWiFiOBD);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 240, 1);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, 255, LV_PART_MAIN);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ==== NEARBY NETWORKS ====
    lv_obj_t *label_nearby = lv_label_create(ui_ScreenPageWiFiOBD);
    lv_label_set_text(label_nearby, "NEARBY");
    lv_obj_set_style_text_font(label_nearby, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_nearby, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(label_nearby, LV_ALIGN_TOP_MID, 0, 134);

    s_list = lv_list_create(ui_ScreenPageWiFiOBD);
    lv_obj_set_size(s_list, 264, 145);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list, 8, LV_PART_MAIN);

    lv_obj_t *label_hint = lv_label_create(ui_ScreenPageWiFiOBD);
    lv_label_set_text(label_hint, "Tap to connect  Slide to back");
    lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_text_align(label_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -15);

    lv_obj_move_foreground(spinner_ring);
    lv_obj_add_event_cb(ui_ScreenPageWiFiOBD, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_ScreenPageWiFiOBD, on_screen_visibility, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_ScreenPageWiFiOBD, on_screen_visibility, LV_EVENT_SCREEN_UNLOADED, NULL);

    s_scanning = false;
}
