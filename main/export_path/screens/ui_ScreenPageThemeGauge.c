// Theme-provided gauge page. Entered by boot (if theme is loaded) or by
// swiping down from the Gear page. Which page is built is driven by
// ui_theme_gauge_page_index, set by the gesture handlers before requesting
// a rebuild — see ui_event_theme_gauge_background in ui.c and boot logic in
// ui_ext.c. The actual widget tree comes entirely from theme_create_page();
// this file just wires it into the existing screen/navigation conventions.
//
// On failure (no theme, page missing, out of memory) ui_ScreenPageThemeGauge
// stays NULL: callers must check it and stay on / fall back to a built-in
// page instead of loading an empty screen.

#include "../ui.h"
#include "theme_engine/theme_interface.h"

static void theme_page_enable_gesture_bubble(lv_obj_t *parent)
{
    uint32_t child_count = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
        lv_obj_add_flag(child, LV_OBJ_FLAG_GESTURE_BUBBLE);
        theme_page_enable_gesture_bubble(child);
    }
}

void ui_ScreenPageThemeGauge_screen_init(void)
{
    const char *page_id = theme_page_list_at(ui_theme_gauge_page_index);
    ui_ScreenPageThemeGauge_attach(page_id ? theme_create_page(page_id) : NULL);
}

void ui_ScreenPageThemeGauge_attach(lv_obj_t *screen)
{
    ui_ScreenPageThemeGauge = screen;
    if (!screen) return;

    // Gesture detection targets the touched child, not the screen. Explicitly
    // bubble gestures through all theme-created objects to this screen.
    lv_obj_add_flag(ui_ScreenPageThemeGauge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_ScreenPageThemeGauge, LV_OBJ_FLAG_GESTURE_BUBBLE);
    theme_page_enable_gesture_bubble(ui_ScreenPageThemeGauge);
    lv_obj_add_event_cb(ui_ScreenPageThemeGauge, ui_event_theme_gauge_background, LV_EVENT_GESTURE, NULL);
}
