// OTA Mode Page
// Shows the device ready for OTA update, with SoftAP + HTTP server running.
// The companion app connects via WiFi (password: 88888888), reads device info
// from GET /ota/info, then uploads firmware/bootmedia via HTTP POST.
// No BLE involvement.

#include "../ui.h"
#include "../ui_ext.h"
#include "app_obd_dsp/device_identity.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

#ifndef OBD_GAUGE_BUILD_TAG
#define OBD_GAUGE_BUILD_TAG "unknown"
#endif

static const char *TAG = "ota_mode";

// UI elements exposed for timer updates
lv_obj_t *ui_ScreenPageOTAMode = NULL;
lv_obj_t *ui_LabelOTAModeVersion = NULL;

// Internal reference to WiFi SSID label for dynamic updates
static lv_obj_t *s_label_wifi_ssid = NULL;

static void ui_event_ota_mode_background(lv_event_t *e);

void ui_ScreenPageOTAMode_screen_init(void)
{
    ui_ScreenPageOTAMode = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageOTAMode, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageOTAMode, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_helpers_style_screen_bg(ui_ScreenPageOTAMode);
    lv_obj_set_style_bg_opa(ui_ScreenPageOTAMode, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // White border ring
    lv_obj_t *spinner_ring = ui_helpers_create_ring(ui_ScreenPageOTAMode, 10);

    // Title: "OTA MODE"
    lv_obj_t *label_title = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_title, "OTA MODE");
    lv_obj_set_style_text_font(label_title, &ui_font_FontTypoderSize20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 40);

    // Pulse icon (animated) to show the device is discoverable
    lv_obj_t *pulse = lv_spinner_create(ui_ScreenPageOTAMode, 1000, 90);
    lv_obj_set_size(pulse, 32, 32);
    lv_obj_align(pulse, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_arc_color(pulse, lv_color_hex(0x00CC66), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(pulse, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pulse, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(pulse, 3, LV_PART_MAIN);

    // Version label (short git hash)
    ui_LabelOTAModeVersion = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(ui_LabelOTAModeVersion, OBD_GAUGE_BUILD_TAG);
    lv_obj_set_style_text_font(ui_LabelOTAModeVersion, &ui_font_FontTypoderSize20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LabelOTAModeVersion, lv_color_hex(0xCCCCCC), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_LabelOTAModeVersion, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(ui_LabelOTAModeVersion, LV_ALIGN_TOP_MID, 0, 120);

    // WiFi AP info (will be updated with real SSID when OTA server starts)
    s_label_wifi_ssid = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(s_label_wifi_ssid, "WiFi: Starting...");
    lv_obj_set_style_text_font(s_label_wifi_ssid, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_label_wifi_ssid, lv_color_hex(0x00CC66), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_label_wifi_ssid, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_label_wifi_ssid, LV_ALIGN_CENTER, 0, 40);

    // Password hint
    lv_obj_t *label_pass = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_pass, "Password: 88888888");
    lv_obj_set_style_text_font(label_pass, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_pass, lv_color_hex(0xFFCC00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label_pass, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_pass, LV_ALIGN_CENTER, 0, 12);

    // Hint: slide to exit
    lv_obj_t *label_hint = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_hint, "Slide away to exit OTA");
    lv_obj_set_style_text_font(label_hint, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_move_foreground(spinner_ring);
    lv_obj_add_event_cb(ui_ScreenPageOTAMode, ui_event_ota_mode_background, LV_EVENT_ALL, NULL);

    // Drop the ELM327 BLE link for the duration of the OTA: the GATT client keeps
    // re-scanning/reconnecting in the background and steals the single 2.4GHz
    // radio from the SoftAP, stalling the upload mid-transfer.
    elm327_ble_pause_for_ota();

    // Give the single 2.4 GHz radio to the OTA SoftAP. Keep the WiFi driver
    // itself alive (the SoftAP reuses it), but stop the ESP-NOW broadcast /
    // presence tasks and deinit only ESP-NOW. Otherwise the master continues a
    // broadcast every 100 ms while TCP is trying to fill the small RX pool.
    espnow_link_pause_for_ota();

    // ota_wifi_server_start() releases BLE (ble_hub_shutdown), video buffers and
    // theme_0 image caches. Theme page objects must be gone before the caches are
    // freed; OTA is entered from the version page, so the theme page is not on screen.
    if (ui_ScreenPageThemeGauge && ui_ScreenPageThemeGauge != lv_scr_act()) {
        lv_obj_del(ui_ScreenPageThemeGauge);
        ui_ScreenPageThemeGauge = NULL;
    }

    // Start WiFi SoftAP + HTTP server immediately
    ESP_LOGI(TAG, "Starting WiFi OTA server from OTA mode screen");
    ota_wifi_info_t info = {0};
    if (!ota_wifi_server_start(&info)) {
        ESP_LOGE(TAG, "Failed to start WiFi OTA server");
        if (s_label_wifi_ssid) {
            lv_label_set_text(s_label_wifi_ssid, "WiFi: Failed to start");
            lv_obj_set_style_text_color(s_label_wifi_ssid, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        // Update SSID label with real WiFi AP name (REFACTOR_PLAN.md P3 #235)
        if (s_label_wifi_ssid) {
            char ssid_text[64];
            snprintf(ssid_text, sizeof(ssid_text), "WiFi: %s", info.ssid);
            lv_label_set_text(s_label_wifi_ssid, ssid_text);
        }
        ESP_LOGI(TAG, "WiFi OTA server started: SSID=%s, IP=%s:%d", info.ssid, info.ip, info.port);
    }
}


// Leaving OTA mode: entering it released the BT controller and quiesced
// ESP-NOW. That is irreversible for this session, so BLE — and with it the OBD
// link — can only come back through a reset.
// Uploads already end in esp_restart(); this covers the "looked and swiped away"
// case so the gauge is never left BLE-dead.
static void exit_ota_mode(void)
{
    ESP_LOGI(TAG, "Exiting OTA mode, stopping WiFi server");
    if (!ota_wifi_server_stop()) {
        ESP_LOGW(TAG, "WiFi OTA start/stop busy; retry exit after it finishes");
        return;
    }

    ESP_LOGI(TAG, "Rebooting to restore BLE (BT controller was released for the OTA radio)");
    // Give the WiFi teardown and the log line a moment to drain.
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

// Any swipe exits OTA mode.  Both former destinations (EasterEgg / Settings)
// are replaced by a reboot, which lands on the normal boot flow.
static void ui_event_ota_mode_background(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT ||
            dir == LV_DIR_TOP  || dir == LV_DIR_BOTTOM) {
            lv_indev_wait_release(lv_indev_get_act());
            exit_ota_mode();
        }
    }

}
