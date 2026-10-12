#pragma once
/*
 * ui_storage_page.h — LVGL storage/partition info page
 *
 * Shows:
 *   - Partition usage (firmware, theme, bootmedia, config)
 *   - Free heap
 *   - SD card status
 *   - Import status
 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create storage info page.
 */
lv_obj_t *ui_storage_page_create(lv_obj_t *parent);

/**
 * Refresh storage info.
 */
void ui_storage_page_refresh(void);

#ifdef __cplusplus
}
#endif
