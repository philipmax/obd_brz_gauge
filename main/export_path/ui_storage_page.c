/*
 * ui_storage_page.c — LVGL storage/partition info page
 */

#include "ui_storage_page.h"
#include "bsp_obd_dsp/sd_card.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ui_storage_page";

static lv_obj_t *s_page = NULL;
static lv_obj_t *s_info_label = NULL;

static void format_size(uint32_t bytes, char *buf, size_t buf_size) {
    if (bytes >= 1024 * 1024) {
        snprintf(buf, buf_size, "%.1f MB", bytes / (1024.0f * 1024.0f));
    } else if (bytes >= 1024) {
        snprintf(buf, buf_size, "%.1f KB", bytes / 1024.0f);
    } else {
        snprintf(buf, buf_size, "%u B", bytes);
    }
}

static void refresh_info(void) {
    if (!s_info_label) {
        return;
    }

    char info[512];
    int len = 0;

    len += snprintf(info + len, sizeof(info) - len, "=== Storage ===\n\n");

    /* Heap */
    uint32_t free_heap = esp_get_free_heap_size();
    char heap_str[32];
    format_size(free_heap, heap_str, sizeof(heap_str));
    len += snprintf(info + len, sizeof(info) - len, "Free Heap: %s\n\n", heap_str);

    /* Partitions */
    const char *part_names[] = {"app0", "app1", "theme_0", "bootmedia", "config"};

    for (int i = 0; i < 5; i++) {
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, part_names[i]);

        if (!part && i < 2) {
            /* Try app partition type */
            part = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, part_names[i]);
        }

        if (part) {
            char size_str[32];
            format_size(part->size, size_str, sizeof(size_str));
            len += snprintf(info + len, sizeof(info) - len,
                           "%s: %s\n", part_names[i], size_str);
        }
    }

    /* SD card */
    len += snprintf(info + len, sizeof(info) - len, "\n=== SD Card ===\n");
    if (sd_card_is_mounted()) {
        len += snprintf(info + len, sizeof(info) - len, "Status: Mounted\n");
        /* TODO: get SD card size/free space */
    } else {
        len += snprintf(info + len, sizeof(info) - len, "Status: Not mounted\n");
    }

    lv_label_set_text(s_info_label, info);
}

static void refresh_btn_cb(lv_event_t *e) {
    refresh_info();
}

lv_obj_t *ui_storage_page_create(lv_obj_t *parent) {
    if (s_page) {
        return s_page;
    }

    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));

    /* Title */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "Storage Info");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    /* Info label */
    s_info_label = lv_label_create(s_page);
    lv_obj_set_size(s_info_label, 180, 150);
    lv_obj_align(s_info_label, LV_ALIGN_TOP_LEFT, 10, 30);
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_12, 0);

    /* Refresh button */
    lv_obj_t *btn = lv_btn_create(s_page);
    lv_obj_set_size(btn, 80, 30);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_event_cb(btn, refresh_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Refresh");
    lv_obj_center(btn_label);

    /* Initial refresh */
    refresh_info();

    return s_page;
}

void ui_storage_page_refresh(void) {
    refresh_info();
}
