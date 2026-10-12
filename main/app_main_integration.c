/*
 * app_main_integration.c — Integration glue for refactored modules
 *
 * This file provides the initialization sequence and wiring between:
 *   - Storage layer (nvs_app, config, sd_card)
 *   - Vehicle system (profile loader, scheduler)
 *   - Network layer (API v1, ESP-NOW v6, BLE hub)
 *   - Theme engine (loader, cache, rules)
 *   - Media system (video player)
 *   - Lifecycle management
 *
 * To integrate into app_main.c:
 *   1. Call refactor_init_storage() early in app_main
 *   2. Call refactor_init_vehicle() after BLE init
 *   3. Call refactor_init_theme() before UI creation
 *   4. Call refactor_init_media() for boot video
 *   5. Replace elm327 poll loop with vp_scheduler integration
 */

#include "bsp_obd_dsp/nvs_app.h"
#include "bsp_obd_dsp/config_part.h"
#include "bsp_obd_dsp/sd_card.h"
#include "bsp_obd_dsp/espnow_v6.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "app_obd_dsp/lifecycle_state.h"
#include "app_obd_dsp/signal_store.h"
#include "app_obd_dsp/sd_import.h"
#include "app_obd_dsp/api_v1_handlers.h"
#include "vehicle/vp_profile.h"
#include "vehicle/vp_scheduler.h"
#include "theme_engine/theme_loader.h"
#include "theme_engine/theme_img_cache.h"
#include "media/video_player.h"
#include "media/esp_new_jpeg_wrapper.h"
#include "esp_log.h"

static const char *TAG = "refactor_integration";

/**
 * Phase 1: Storage layer initialization
 * Call early in app_main, before NVS config access.
 */
esp_err_t refactor_init_storage(void) {
    esp_err_t err;

    ESP_LOGI(TAG, "=== Storage Init ===");

    /* Initialize nvs_app (application private NVS) */
    err = nvs_app_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_app init failed: %d", err);
        return err;
    }

    /* Initialize config partition (LittleFS) */
    err = config_part_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_part init failed: %d", err);
        /* Non-fatal: device can run without config partition */
    }

    /* Initialize SD card */
    err = sd_card_init();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "SD card not available (non-fatal)");
        /* Non-fatal: device can run without SD */
    }

    /* Initialize signal store */
    err = signal_store_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "signal_store init failed: %d", err);
        return err;
    }

    /* Initialize lifecycle state machine */
    err = lifecycle_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lifecycle init failed: %d", err);
        return err;
    }

    lifecycle_post_event(LIFECYCLE_EVENT_INIT_DONE);

    ESP_LOGI(TAG, "Storage init complete");
    return ESP_OK;
}

/**
 * Phase 2: Vehicle system initialization
 * Call after BLE initialization.
 */
esp_err_t refactor_init_vehicle(void) {
    esp_err_t err;

    ESP_LOGI(TAG, "=== Vehicle System Init ===");

    /* Initialize BLE hub (coordinates ELM327/RaceChrono/ESP-NOW) */
    err = ble_hub_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble_hub init failed: %d", err);
        return err;
    }

    /* Load active vehicle profile */
    char profile_id[64] = "generic";
    nvs_app_get_str("veh", "active", profile_id, sizeof(profile_id));

    ESP_LOGI(TAG, "Loading vehicle profile: %s", profile_id);

    /* TODO: Load profile from /config/vehicles/<profile_id>.json
     * For now, use builtin generic profile */

    /* Initialize ESP-NOW v6 */
    err = espnow_v6_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "espnow_v6 init failed: %d (non-fatal)", err);
    }

    ESP_LOGI(TAG, "Vehicle system init complete");
    return ESP_OK;
}

/**
 * Phase 3: Theme engine initialization
 * Call before LVGL UI creation.
 */
esp_err_t refactor_init_theme(void) {
    esp_err_t err;

    ESP_LOGI(TAG, "=== Theme Engine Init ===");

    /* Initialize theme loader */
    err = theme_loader_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "theme_loader init failed: %d", err);
        return err;
    }

    /* Initialize image cache */
    err = theme_img_cache_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "theme_img_cache init failed: %d", err);
        return err;
    }

    /* Load theme from slot 0 */
    const nvs_user_cfg_t *cfg = nvs_cfg_get();
    uint8_t theme_slot = cfg ? cfg->theme_slot : 0;

    ESP_LOGI(TAG, "Loading theme from slot %u", theme_slot);
    err = theme_loader_load(theme_slot);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Theme load failed, using minimal theme");
    }

    ESP_LOGI(TAG, "Theme engine init complete");
    return ESP_OK;
}

/**
 * Phase 4: Media system initialization
 * Call for boot video playback.
 */
esp_err_t refactor_init_media(void) {
    esp_err_t err;

    ESP_LOGI(TAG, "=== Media System Init ===");

    /* Initialize JPEG decoder */
    err = jpeg_decoder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decoder init failed: %d", err);
        return err;
    }

    /* Run self-test */
    err = jpeg_decoder_self_test();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decoder self-test failed: %d", err);
        return err;
    }

    /* Initialize video player */
    err = video_player_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Video player init failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Media system init complete");
    return ESP_OK;
}

/**
 * Integration: Replace elm327_ble_client.c poll loop with vp_scheduler
 *
 * In elm327_ble_client.c, replace the 12-slot switch with:
 *
 * static vp_scheduler_t s_scheduler = {0};
 *
 * // In do_elm_init():
 * const vp_profile_t *profile = ...; // Load active profile
 * vp_scheduler_init(&s_scheduler, profile);
 *
 * // In obd_poll_task(), replace switch(tick_count):
 * if (vp_scheduler_tick(&s_scheduler)) {
 *     const char *cmd = vp_scheduler_get_next_cmd(&s_scheduler);
 *     if (cmd) {
 *         elm327_ble_send_ascii_blocking(cmd);
 *     }
 * }
 *
 * // In notify callback:
 * vp_scheduler_feed_response(&s_scheduler, data, len);
 */

/**
 * Integration: Register API v1 handlers with HTTP server
 *
 * In ota_wifi_server.c, after httpd_start():
 *
 * api_v1_register_handlers(server);
 */

/**
 * Integration: Auto-import from SD on boot
 *
 * In app_main(), after refactor_init_storage():
 *
 * int imported = sd_import_auto_boot();
 * if (imported > 0) {
 *     ESP_LOGI(TAG, "Auto-imported %d items from SD", imported);
 * }
 */

/**
 * Cleanup: Free resources before OTA
 *
 * Call before entering WiFi OTA mode to free PSRAM:
 */
void refactor_cleanup_for_ota(void) {
    ESP_LOGI(TAG, "Cleanup for OTA");

    /* Stop video player */
    video_player_stop();

    /* Clear theme cache */
    theme_img_cache_clear_all();

    /* Unload theme */
    theme_loader_unload();

    /* Post lifecycle event */
    lifecycle_post_event(LIFECYCLE_EVENT_OTA_WIFI_START);
}

/**
 * Example: Play boot video
 */
esp_err_t refactor_play_boot_video(void) {
    video_config_t config = {
        .mode = VIDEO_MODE_FULLSCREEN,
        .source = VIDEO_SRC_FLASH,  /* Try flash first */
        .path = NULL,
        .target_fps = 24,
        .loop = false,
        .video_view = NULL,
    };

    esp_err_t err = video_player_start(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot video playback failed: %d", err);
        /* Try SD fallback */
        config.source = VIDEO_SRC_SD;
        config.path = "/sd/OBD/BOOT/boot.obv";
        err = video_player_start(&config);
    }

    return err;
}
