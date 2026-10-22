// Vehicle Page (entered from the VEHICLE button on the settings page)
//  - Roller lists the built-in obd2.generic plus every installed profile (vp_manager_list).
//  - USE: mark the selected profile PENDING; it is tried on the next boot.
//  - Generic uses the same roller + USE path as imported profiles; it also
//    cancels any deferred legacy-vehicle migration. No separate reset/undo buttons.
//  A pending change shows the REBOOT button. The status line shows the running
//  profile's state / safe mode; failures show vp_manager_last_error().
//  Swipe up/left/right returns to the settings page.

#include "../ui.h"
#include <stdio.h>
#include <string.h>
#include "vehicle/vp_manager.h"
#include "app_obd_dsp/first_boot_migration.h"
#include "esp_system.h"

#define VEH_LIST_MAX 16

static lv_obj_t *s_lbl_active = NULL;
static lv_obj_t *s_lbl_state  = NULL;
static lv_obj_t *s_roller     = NULL;
static lv_obj_t *s_lbl_entry  = NULL;
static lv_obj_t *s_lbl_msg    = NULL;
static lv_obj_t *s_btn_reboot = NULL;

static vpm_entry_t s_entries[VEH_LIST_MAX];
static int s_entry_count = 0;

static const char *veh_state_text(veh_state_t st)
{
    switch (st) {
    case VEH_STATE_INSTALLED: return "INSTALLED";
    case VEH_STATE_PENDING:   return "PENDING";
    case VEH_STATE_ACTIVE:    return "ACTIVE";
    case VEH_STATE_FAILED:    return "FAILED";
    default:                  return "";
    }
}

static lv_color_t veh_state_color(veh_state_t st)
{
    switch (st) {
    case VEH_STATE_ACTIVE:  return lv_color_hex(0x00CC66);
    case VEH_STATE_PENDING: return lv_color_hex(0xFFCC00);
    case VEH_STATE_FAILED:  return lv_color_hex(0xFF4040);
    default:                return lv_color_hex(0x888888);
    }
}

static void veh_set_msg(const char *text, bool ok)
{
    lv_label_set_text(s_lbl_msg, text ? text : "");
    lv_obj_set_style_text_color(s_lbl_msg, ok ? lv_color_hex(0x00CC66) : lv_color_hex(0xFF4040), LV_PART_MAIN);
}

static void veh_update_entry_line(void)
{
    int sel = (int)lv_roller_get_selected(s_roller);
    if (sel < 0 || sel >= s_entry_count) {
        lv_label_set_text(s_lbl_entry, "");
        return;
    }
    const vpm_entry_t *e = &s_entries[sel];
    char line[48];
    if (e->builtin) {
        snprintf(line, sizeof(line), "BUILT-IN%s", e->pending ? "  NEXT BOOT" : e->active ? "  RUNNING" : "");
    } else if (e->pending) {
        snprintf(line, sizeof(line), "REV %u  NEXT BOOT", (unsigned)e->rev);
    } else {
        snprintf(line, sizeof(line), "REV %u  %s%s", (unsigned)e->rev,
                 e->active ? "RUNNING " : "", veh_state_text(e->state));
    }
    lv_label_set_text(s_lbl_entry, line);
}

// Re-read everything from vp_manager: the page is created once and re-shown, and
// profiles can be installed from web/App or SD in between.
static void veh_page_refresh(void)
{
    const vp_profile_t *vp = vp_manager_active();
    lv_label_set_text(s_lbl_active, vp->name);

    veh_state_t st = vp_manager_active_state();
    if (vp_manager_safe_mode()) {
        lv_label_set_text(s_lbl_state, "SAFE MODE");
        lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(0xFF4040), LV_PART_MAIN);
    } else if (strcmp(vp_manager_active_id(), VPM_GENERIC_ID) == 0) {
        lv_label_set_text(s_lbl_state, "GENERIC");
        lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(0x888888), LV_PART_MAIN);
    } else {
        lv_label_set_text(s_lbl_state, veh_state_text(st));
        lv_obj_set_style_text_color(s_lbl_state, veh_state_color(st), LV_PART_MAIN);
    }

    s_entry_count = vp_manager_list(s_entries, VEH_LIST_MAX);
    if (s_entry_count < 0) s_entry_count = 0;

    // Roller options: one name per line (a newline would split an entry).
    char opts[VEH_LIST_MAX * (VPM_NAME_LEN + 1)];
    size_t pos = 0;
    int select = 0;
    bool pending = false;
    for (int i = 0; i < s_entry_count; i++) {
        const vpm_entry_t *e = &s_entries[i];
        if (i > 0) opts[pos++] = '\n';
        for (const char *c = e->name[0] ? e->name : e->id; *c && pos < sizeof(opts) - 2; c++) {
            opts[pos++] = (*c == '\n') ? ' ' : *c;
        }
        if (e->pending) { select = i; pending = true; }
        else if (e->active && !pending) select = i;
    }
    opts[pos] = '\0';
    if (s_entry_count == 0) strcpy(opts, "Generic OBD2");

    lv_roller_set_options(s_roller, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_roller, (uint16_t)select, LV_ANIM_OFF);
    veh_update_entry_line();

    if (pending) lv_obj_clear_flag(s_btn_reboot, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_btn_reboot, LV_OBJ_FLAG_HIDDEN);

    const char *err = vp_manager_last_error();
    veh_set_msg((err && err[0]) ? err : "", false);
}

// Refresh the page, then report the action result (overrides the stale last_error line).
static void veh_show_result(esp_err_t err, const char *ok_text)
{
    veh_page_refresh();
    if (err == ESP_OK) {
        veh_set_msg(ok_text, true);
    } else {
        const char *why = vp_manager_last_error();
        veh_set_msg((why && why[0]) ? why : esp_err_to_name(err), false);
    }
}

static void on_roller_change(lv_event_t *e)
{
    (void)e;
    veh_update_entry_line();
}

static void on_use_click(lv_event_t *e)
{
    (void)e;
    int sel = (int)lv_roller_get_selected(s_roller);
    if (sel < 0 || sel >= s_entry_count) return;
    const vpm_entry_t *ent = &s_entries[sel];
    bool generic = strcmp(ent->id, VPM_GENERIC_ID) == 0;
    bool pending = false;
    for (int i = 0; i < s_entry_count; i++) {
        if (s_entries[i].pending) pending = true;
    }
    bool running = ent->active && !vp_manager_safe_mode()
                   && vp_manager_active_state() != VEH_STATE_FAILED;
    // Re-selecting the running profile must still cancel a pending switch.
    // Generic must also clear deferred migration, even when already running.
    if (running && !pending && !generic) {
        veh_set_msg("Already running", true);
        return;
    }
    char id[VPM_ID_LEN];
    strlcpy(id, ent->id, sizeof(id));   // s_entries is rebuilt by the refresh below
    esp_err_t err = vp_manager_activate(id);
    if (err == ESP_OK && generic) first_boot_migration_drop_vehicle();
    veh_show_result(err, running ? "Already running" : "Reboot to apply");
}

static void on_reboot_click(lv_event_t *e)
{
    (void)e;
    esp_restart();
}

static void on_screen_load_start(lv_event_t *e)
{
    (void)e;
    veh_page_refresh();
}

static lv_obj_t *make_veh_btn(lv_obj_t *parent, const char *txt, lv_coord_t w, lv_coord_t x, lv_coord_t y,
                              lv_color_t bg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, 30);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN);
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
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void ui_ScreenPageVehicle_screen_init(void)
{
    ui_ScreenPageVehicle = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageVehicle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageVehicle, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageVehicle);
    lv_obj_set_style_bg_opa(ui_ScreenPageVehicle, 255, LV_PART_MAIN);

    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageVehicle, 10);

    // Black ear image at top (created before the widgets so they draw on top of the notch)
    lv_obj_t *ear = lv_img_create(ui_ScreenPageVehicle);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_set_width(ear, LV_SIZE_CONTENT);
    lv_obj_set_height(ear, LV_SIZE_CONTENT);
    lv_obj_set_pos(ear, 0, -142);
    lv_obj_set_align(ear, LV_ALIGN_CENTER);
    lv_obj_add_flag(ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ear, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(ui_ScreenPageVehicle);
    lv_label_set_text(title, "VEHICLE");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -112);

    // Running profile + its state
    s_lbl_active = lv_label_create(ui_ScreenPageVehicle);
    lv_obj_set_style_text_font(s_lbl_active, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_active, ui_theme_color_lv(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_label_set_long_mode(s_lbl_active, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_active, 240);
    lv_obj_set_style_text_align(s_lbl_active, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_lbl_active, LV_ALIGN_CENTER, 0, -84);

    s_lbl_state = lv_label_create(ui_ScreenPageVehicle);
    lv_obj_set_style_text_font(s_lbl_state, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_align(s_lbl_state, LV_ALIGN_CENTER, 0, -62);

    // Installed profiles
    s_roller = lv_roller_create(ui_ScreenPageVehicle);
    lv_obj_set_style_clip_corner(s_roller, true, 0);
    lv_obj_clear_flag(s_roller, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_roller_set_options(s_roller, "Generic OBD2", LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(s_roller, 220);
    ui_helpers_style_dark_roller(s_roller, &ui_font_FontTypoderSize20);
    lv_roller_set_visible_row_count(s_roller, 1);   // after the font so the row height uses Size20
    lv_obj_set_height(s_roller, 30);
    lv_obj_align(s_roller, LV_ALIGN_CENTER, 0, -26);
    lv_obj_add_event_cb(s_roller, on_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_entry = lv_label_create(ui_ScreenPageVehicle);
    lv_obj_set_style_text_font(s_lbl_entry, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_entry, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(s_lbl_entry, LV_ALIGN_CENTER, 0, 2);

    // One action for every roller entry, including the built-in Generic OBD2.
    make_veh_btn(ui_ScreenPageVehicle, "USE", 110, 0, 34, lv_color_hex(0x00AA55), on_use_click);

    // Result / last error
    s_lbl_msg = lv_label_create(ui_ScreenPageVehicle);
    lv_obj_set_style_text_font(s_lbl_msg, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_long_mode(s_lbl_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_msg, 250);
    lv_obj_set_style_text_align(s_lbl_msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_lbl_msg, LV_ALIGN_CENTER, 0, 76);

    s_btn_reboot = make_veh_btn(ui_ScreenPageVehicle, "REBOOT", 110, 0, 116, lv_color_hex(0xAA7700), on_reboot_click);
    lv_obj_add_flag(s_btn_reboot, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = lv_label_create(ui_ScreenPageVehicle);
    lv_label_set_text(hint, "Swipe: back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 150);

    veh_page_refresh();

    lv_obj_move_foreground(ring);
    lv_obj_add_event_cb(ui_ScreenPageVehicle, on_screen_load_start, LV_EVENT_SCREEN_LOAD_START, NULL);
    lv_obj_add_event_cb(ui_ScreenPageVehicle, ui_event_vehicle_background, LV_EVENT_GESTURE, NULL);
}
