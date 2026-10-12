#pragma once
/*
 * ui_vehicle_page.h — LVGL vehicle profile selection page
 *
 * Replaces hardcoded vehicle index selection with:
 *   - List of available profiles (builtin + imported)
 *   - Profile details display
 *   - Import from SD button
 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create vehicle profile selection page.
 * Returns the page object.
 */
lv_obj_t *ui_vehicle_page_create(lv_obj_t *parent);

/**
 * Refresh profile list (call after import).
 */
void ui_vehicle_page_refresh(void);

/**
 * Show profile details.
 */
void ui_vehicle_page_show_details(const char *profile_id);

#ifdef __cplusplus
}
#endif
