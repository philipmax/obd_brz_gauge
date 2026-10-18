#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"

extern lv_obj_t *ui_ScreenPageTempCustom;
static lv_obj_t *s_temp_rollers[3] = {NULL, NULL, NULL};

// Roller position <-> disp_item_t mapping is explicit: DISP_ITEM_RESERVED_9 (retired) is not listed,
// so from BOOST on the position differs from the item value. k_data_options lists labels in the same order.
static const uint8_t k_temp_sources[] = {
    DISP_ITEM_CLT, DISP_ITEM_IAT, DISP_ITEM_OIL, DISP_ITEM_LOAD, DISP_ITEM_TPS, DISP_ITEM_RPM,
    DISP_ITEM_SPEED, DISP_ITEM_BAT, DISP_ITEM_OILP, DISP_ITEM_BOOST, DISP_ITEM_AFR,
};
#define TEMP_SOURCE_COUNT (sizeof(k_temp_sources) / sizeof(k_temp_sources[0]))

static const char *k_data_options =
    "CLT\n"
    "IAT\n"
    "OIL\n"
    "LOAD\n"
    "TPS\n"
    "RPM\n"
    "SPEED\n"
    "BAT\n"
    "OILP\n"
    "BOOST\n"
    "AFR";

// Per-row default item; also used for a stored value that is not selectable (same rule as nvs_storage_init)
static const uint8_t k_temp_default_items[3] = NVS_TEMP_MAP_DEFAULT;

static uint16_t temp_item_to_pos(uint8_t item, int row)
{
    if (!ui_disp_item_is_selectable(item)) item = k_temp_default_items[row];
    for (uint16_t i = 0; i < TEMP_SOURCE_COUNT; i++) {
        if (k_temp_sources[i] == item) return i;
    }
    return 0;
}

static void on_temp_map_changed(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= 3) return;

    uint16_t pos = lv_roller_get_selected(s_temp_rollers[idx]);
    if (pos >= TEMP_SOURCE_COUNT) return;

    nvs_user_cfg_t base;
    nvs_cfg_snapshot(&base);
    nvs_user_cfg_t cfg = base;
    cfg.temp_display_map[idx] = k_temp_sources[pos];
    nvs_cfg_update(&base, &cfg);
}

static void create_row(lv_obj_t *parent, int idx, const char *title, lv_coord_t y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, -92, y);

    s_temp_rollers[idx] = lv_roller_create(parent);
    lv_obj_clear_flag(s_temp_rollers[idx], LV_OBJ_FLAG_GESTURE_BUBBLE); // scrolling the selection must not trigger the page gesture
    lv_roller_set_options(s_temp_rollers[idx], k_data_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_temp_rollers[idx], 1);
    lv_obj_set_width(s_temp_rollers[idx], 170);
    lv_obj_set_height(s_temp_rollers[idx], 42);
    lv_obj_set_style_text_font(s_temp_rollers[idx], &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_temp_rollers[idx], &ui_font_FontTypoderSize20, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_temp_rollers[idx], lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_temp_rollers[idx], 255, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_temp_rollers[idx], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_temp_rollers[idx], lv_color_hex(0xFFFFFF), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_temp_rollers[idx], lv_color_hex(0x000000), LV_PART_SELECTED);
    lv_obj_set_style_radius(s_temp_rollers[idx], 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_temp_rollers[idx], 8, LV_PART_SELECTED);   // round the white selected band too, it was spilling square past the corners
    lv_obj_align(s_temp_rollers[idx], LV_ALIGN_CENTER, 34, y);
    lv_obj_add_event_cb(s_temp_rollers[idx], on_temp_map_changed, LV_EVENT_VALUE_CHANGED, (void *)idx);
}

void ui_ScreenPageTempCustom_screen_init(void)
{
    nvs_user_cfg_t cfg = nvs_cfg_read();

    ui_ScreenPageTempCustom = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageTempCustom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageTempCustom, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageTempCustom);
    lv_obj_set_style_bg_opa(ui_ScreenPageTempCustom, 255, LV_PART_MAIN);

    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageTempCustom, 10);   // white ring: static circular border, replaces the rotating spinner, removes the arc seam gap

    create_row(ui_ScreenPageTempCustom, 0, "ROW 1", -72);
    create_row(ui_ScreenPageTempCustom, 1, "ROW 2", -18);
    create_row(ui_ScreenPageTempCustom, 2, "ROW 3", 36);

    for (int i = 0; i < 3; ++i) {
        lv_roller_set_selected(s_temp_rollers[i], temp_item_to_pos(cfg.temp_display_map[i], i), LV_ANIM_OFF);
    }

    lv_obj_t *hint = lv_label_create(ui_ScreenPageTempCustom);
    lv_label_set_text(hint, "Swipe up/left/right to return");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 92);

    lv_obj_t *ear = lv_img_create(ui_ScreenPageTempCustom);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_align(ear, LV_ALIGN_CENTER, 0, -142);

    lv_obj_move_foreground(ring);   // bring the ring to the front
    lv_obj_add_event_cb(ui_ScreenPageTempCustom, ui_event_temp_custom_background, LV_EVENT_GESTURE, NULL);
}
