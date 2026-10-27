// Connection type chooser: swipe up from the version/EasterEgg page.
// Two entries: BLE SCAN (existing page) and WIFI OBD (ELM327 over TCP).
// Slide left/right returns to the version page.

#include "../ui.h"

static void goto_ble(lv_event_t *e)
{
    (void)e;
    _ui_screen_change(&ui_ScreenPageBLEScan, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                      &ui_ScreenPageBLEScan_screen_init);
}

static void goto_wifi(lv_event_t *e)
{
    (void)e;
    _ui_screen_change(&ui_ScreenPageWiFiOBD, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                      &ui_ScreenPageWiFiOBD_screen_init);
}

static void on_gesture(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_get_act());
        _ui_screen_change(&ui_ScreenPageEasterEgg, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                          &ui_ScreenPageEasterEgg_screen_init);
    }
}

void ui_ScreenPageConnSelect_screen_init(void)
{
    ui_ScreenPageConnSelect = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageConnSelect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageConnSelect, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_helpers_style_screen_bg(ui_ScreenPageConnSelect);
    lv_obj_set_style_bg_opa(ui_ScreenPageConnSelect, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ScreenPageConnSelect, 0, LV_PART_MAIN);

    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageConnSelect, 10);

    lv_obj_t *title = lv_label_create(ui_ScreenPageConnSelect);
    lv_label_set_text(title, "CONNECTION");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *sub = lv_label_create(ui_ScreenPageConnSelect);
    lv_label_set_text(sub, "Choose OBD link");
    lv_obj_set_style_text_font(sub, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *list = lv_list_create(ui_ScreenPageConnSelect);
    lv_obj_set_size(list, 264, 120);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 8, LV_PART_MAIN);

    lv_obj_t *b1 = lv_list_add_btn(list, LV_SYMBOL_BLUETOOTH, "BLE SCAN");
    lv_obj_set_style_bg_color(b1, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b1, 255, LV_PART_MAIN);
    lv_obj_set_style_text_color(b1, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(b1, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_add_event_cb(b1, goto_ble, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b2 = lv_list_add_btn(list, LV_SYMBOL_WIFI, "WIFI OBD");
    lv_obj_set_style_bg_color(b2, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b2, 255, LV_PART_MAIN);
    lv_obj_set_style_text_color(b2, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(b2, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_add_event_cb(b2, goto_wifi, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = lv_label_create(ui_ScreenPageConnSelect);
    lv_label_set_text(hint, "Tap to open  Slide to back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -15);

    lv_obj_move_foreground(ring);
    lv_obj_add_event_cb(ui_ScreenPageConnSelect, on_gesture, LV_EVENT_GESTURE, NULL);
}
