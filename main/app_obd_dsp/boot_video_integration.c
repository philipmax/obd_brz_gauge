/**
 * boot_video_integration.c
 *
 * Boot video source strategy: SD优先，旧格式其次，内置Logo最后
 * Integrates OBV1 video player with boot sequence
 */

#include "boot_video_integration.h"
#include "../media/video_player.h"
#include "../media/obv1_parser.h"
#include "boot_media_mount.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "boot_video";

typedef enum {
    BOOT_VIDEO_SOURCE_SD = 0,      // SD:/OBD/BOOT/boot.obv
    BOOT_VIDEO_SOURCE_LEGACY,      // bootmedia partition (old delta_varint format)
    BOOT_VIDEO_SOURCE_BUILTIN      // Built-in logo animation
} boot_video_source_t;

static boot_video_source_t s_source = BOOT_VIDEO_SOURCE_BUILTIN;
static bool s_initialized = false;

static bool try_sd_boot_video(void)
{
    // Try to open SD:/OBD/BOOT/boot.obv
    FILE *f = fopen("/sdcard/OBD/BOOT/boot.obv", "rb");
    if (!f) {
        ESP_LOGD(TAG, "No SD boot video");
        return false;
    }

    // Validate OBV1 container
    obv1_header_t header;
    if (obv1_parse_header(f, &header) != ESP_OK) {
        ESP_LOGW(TAG, "SD boot video invalid OBV1 header");
        fclose(f);
        return false;
    }

    fclose(f);

    ESP_LOGI(TAG, "Using SD boot video: %dx%d, %d fps, %d frames",
             header.width, header.height, header.fps, header.frame_count);
    return true;
}

static bool try_legacy_boot_media(void)
{
    // Check if bootmedia partition has valid legacy animation
    boot_media_manifest_t manifest;
    if (boot_media_mount_get_manifest(&manifest) != ESP_OK) {
        ESP_LOGD(TAG, "No legacy bootmedia");
        return false;
    }

    if (manifest.format != BOOT_MEDIA_FORMAT_DELTA_VARINT) {
        ESP_LOGD(TAG, "Bootmedia not legacy format");
        return false;
    }

    ESP_LOGI(TAG, "Using legacy bootmedia animation");
    return true;
}

esp_err_t boot_video_integration_init(void)
{
    if (s_initialized) return ESP_OK;

    // Strategy: SD > Legacy > Builtin
    if (try_sd_boot_video()) {
        s_source = BOOT_VIDEO_SOURCE_SD;
    } else if (try_legacy_boot_media()) {
        s_source = BOOT_VIDEO_SOURCE_LEGACY;
    } else {
        s_source = BOOT_VIDEO_SOURCE_BUILTIN;
        ESP_LOGI(TAG, "Using built-in logo animation");
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t boot_video_integration_play(void)
{
    switch (s_source) {
        case BOOT_VIDEO_SOURCE_SD: {
            // Play SD OBV1 video
            video_play_config_t cfg = {
                .mode = VIDEO_MODE_FULLSCREEN,
                .loop = false,
                .fps = 24  // Will be overridden by OBV1 header
            };
            return video_player_open("/sdcard/OBD/BOOT/boot.obv", &cfg);
        }

        case BOOT_VIDEO_SOURCE_LEGACY: {
            // Use legacy boot_block_player (stream from flash)
            // Legacy player should be modified to stream from flash instead of
            // loading entire animation into PSRAM
            ESP_LOGI(TAG, "Playing legacy bootmedia (streaming mode)");
            // boot_block_player_play(); // To be implemented
            return ESP_OK;
        }

        case BOOT_VIDEO_SOURCE_BUILTIN:
        default: {
            // Show built-in logo (handled by ui_ext.c intro sequence)
            ESP_LOGI(TAG, "Showing built-in logo");
            return ESP_OK;
        }
    }
}

void boot_video_integration_stop(void)
{
    if (s_source == BOOT_VIDEO_SOURCE_SD) {
        video_player_stop();
    }
}

boot_video_source_t boot_video_integration_get_source(void)
{
    return s_source;
}
