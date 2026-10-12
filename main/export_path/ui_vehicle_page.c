/*
 * ui_vehicle_page.c — LVGL vehicle profile selection page
 */

#include "ui_vehicle_page.h"
#include "ui_disp_item.h"
#include "vehicle/vp_profile.h"
#include "bsp_obd_dsp/nvs_app.h"
#include "bsp_obd_dsp/config_part.h"
#include "esp_log.h"
#include <string.h>
#include <dirent.h>

static const char *TAG = "ui_vehicle_page";

static lv_obj_t *s_page = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_details_label = NULL;
static lv_obj_t *s_import_btn = NULL;

#define MAX_PROFILES 32

typedef struct {
    char id[64];
    char name[64];
    bool is_builtin;
} profile_item_t;

static profile_item_t s_profiles[MAX_PROFILES];
static uint8_t s_profile_count = 0;

static void scan_profiles(void) {
    s_profile_count = 0;

    /* Add builtin generic */
    profile_item_t *item = &s_profiles[s_profile_count++];
    strncpy(item->id, "generic", sizeof(item->id) - 1);
    strncpy(item->name, "OBD2 Generic", sizeof(item->name) - 1);
    item->is_builtin = true;

    /* Scan /config/vehicles/ for imported profiles */
    if (!config_part_is_mounted()) {
        return;
    }

    DIR *dir = opendir("/config/vehicles");
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_profile_count < MAX_PROFILES) {
        if (entry->d_type != DT_REG) {
            continue;
        }

        /* Check .json extension */
        size_t len = strlen(entry->d_name);
        if (len < 6 || strcmp(entry->d_name + len - 5, ".json") != 0) {
            continue;
        }

        /* Extract ID (filename without .json) */
        item = &s_profiles[s_profile_count];
        strncpy(item->id, entry->d_name, sizeof(item->id) - 1);
        item->id[len - 5] = '\0'; /* Remove .json */

        /* Use ID as name for now (TODO: parse name from JSON) */
        strncpy(item->name, item->id, sizeof(item->name) - 1);
        item->is_builtin = false;

        s_profile_count++;
    }

    closedir(dir);

    ESP_LOGI(TAG, "Found %u profiles", s_profile_count);
}

static void profile_click_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

    if (idx >= s_profile_count) {
        return;
    }

    profile_item_t *item = &s_profiles[idx];

    /* Save to NVS */
    nvs_app_set_str("veh", "active", item->id);

    /* Update details */
    char details[256];
    snprintf(details, sizeof(details),
             "Active: %s\n%s\nRestart to apply.",
             item->name,
             item->is_builtin ? "(Built-in)" : "(Imported)");

    lv_label_set_text(s_details_label, details);

    ESP_LOGI(TAG, "Selected profile: %s", item->id);
}

static void import_click_cb(lv_event_t *e) {
    /* TODO: trigger SD import scan and show import UI */
    ESP_LOGI(TAG, "Import button clicked");
}

static void create_profile_list(void) {
    if (s_list) {
        lv_obj_del(s_list);
    }

    s_list = lv_list_create(s_page);
    lv_obj_set_size(s_list, 180, 140);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 30);

    /* Get active profile */
    char active_id[64] = "generic";
    nvs_app_get_str("veh", "active", active_id, sizeof(active_id));

    /* Add profile buttons */
    for (uint8_t i = 0; i < s_profile_count; i++) {
        profile_item_t *item = &s_profiles[i];

        lv_obj_t *btn = lv_list_add_btn(s_list, NULL, item->name);
        lv_obj_add_event_cb(btn, profile_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        /* Highlight active */
        if (strcmp(item->id, active_id) == 0) {
            lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
        }
    }
}

lv_obj_t *ui_vehicle_page_create(lv_obj_t *parent) {
    if (s_page) {
        return s_page;
    }

    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "Vehicle Profile");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    /* Scan profiles */
    scan_profiles();

    /* Create profile list */
    create_profile_list();

    /* Details label */
    s_details_label = lv_label_create(s_page);
    lv_label_set_text(s_details_label, "Select a profile");
    lv_obj_set_width(s_details_label, 180);
    lv_obj_align(s_details_label, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_label_set_long_mode(s_details_label, LV_LABEL_LONG_WRAP);

    /* Import button */
    s_import_btn = lv_btn_create(s_page);
    lv_obj_set_size(s_import_btn, 120, 30);
    lv_obj_align(s_import_btn, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_event_cb(s_import_btn, import_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(s_import_btn);
    lv_label_set_text(btn_label, "Import from SD");
    lv_obj_center(btn_label);

    return s_page;
}

void ui_vehicle_page_refresh(void) {
    scan_profiles();
    create_profile_list();
}

void ui_vehicle_page_show_details(const char *profile_id) {
    if (!profile_id || !s_details_label) {
        return;
    }

    /* Find profile */
    for (uint8_t i = 0; i < s_profile_count; i++) {
        if (strcmp(s_profiles[i].id, profile_id) == 0) {
            char details[256];
            snprintf(details, sizeof(details), "%s\n%s",
                     s_profiles[i].name,
                     s_profiles[i].is_builtin ? "Built-in" : "Imported");
            lv_label_set_text(s_details_label, details);
            return;
        }
    }
}
