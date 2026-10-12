/*
 * video_player.c — Video playback engine
 */

#include "video_player.h"
#include "obv1_parser.h"
#include "esp_new_jpeg_wrapper.h"
#include "bsp_obd_dsp/sd_card.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "video_player";

#define RING_BUFFER_SIZE (1280 * 1024)  /* 1.25MB */
#define DMA_BUFFER_SIZE (16 * 1024)     /* 16KB internal RAM for SD reads */
#define FRAMEBUFFER_SIZE (360 * 360 * 2) /* RGB565 */

/* Player context */
static struct {
    video_config_t config;
    video_state_t state;

    obv1_context_t obv1;
    uint32_t current_frame;

    /* Ring buffer */
    uint8_t *ring_buffer;
    uint32_t ring_write_pos;
    uint32_t ring_read_pos;
    uint32_t ring_bytes_used;
    SemaphoreHandle_t ring_mutex;

    /* Framebuffers (composite mode) */
    uint16_t *fb[2];
    uint8_t fb_index;

    /* Tasks */
    TaskHandle_t read_task;
    TaskHandle_t decode_task;

    /* Statistics */
    video_stats_t stats;

    bool stop_requested;
} s_player = {0};

/* Read task: load frames from source into ring buffer */
static void read_task_func(void *arg) {
    ESP_LOGI(TAG, "Read task started");

    while (!s_player.stop_requested) {
        /* TODO: Implement ring buffer loading
         * 1. Check ring buffer space
         * 2. Read next frame from SD or flash
         * 3. Write to ring buffer
         * 4. Update write position
         */

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Read task stopped");
    vTaskDelete(NULL);
}

/* Decode task: decode frames from ring buffer and display */
static void decode_task_func(void *arg) {
    ESP_LOGI(TAG, "Decode task started");

    while (!s_player.stop_requested) {
        /* TODO: Implement frame decode and display
         * 1. Check if frame available in ring buffer
         * 2. Decode JPEG
         * 3. Display (fullscreen: block decode, composite: whole frame to fb)
         * 4. Update read position
         * 5. Frame timing control
         */

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Decode task stopped");
    vTaskDelete(NULL);
}

esp_err_t video_player_init(void) {
    if (s_player.state != VIDEO_STATE_STOPPED) {
        return ESP_OK;
    }

    /* Initialize JPEG decoder */
    esp_err_t err = jpeg_decoder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decoder init failed: %d", err);
        return err;
    }

    /* Allocate ring buffer in PSRAM */
    s_player.ring_buffer = heap_caps_malloc(RING_BUFFER_SIZE,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_player.ring_buffer) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer");
        return ESP_ERR_NO_MEM;
    }

    /* Create mutex */
    s_player.ring_mutex = xSemaphoreCreateMutex();
    if (!s_player.ring_mutex) {
        heap_caps_free(s_player.ring_buffer);
        return ESP_ERR_NO_MEM;
    }

    memset(&s_player.stats, 0, sizeof(s_player.stats));
    s_player.state = VIDEO_STATE_STOPPED;

    ESP_LOGI(TAG, "Video player initialized");
    return ESP_OK;
}

esp_err_t video_player_start(const video_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_player.state != VIDEO_STATE_STOPPED) {
        video_player_stop();
    }

    s_player.config = *config;
    s_player.state = VIDEO_STATE_LOADING;
    s_player.stop_requested = false;

    /* Open video source */
    if (config->source == VIDEO_SRC_SD) {
        if (!config->path) {
            return ESP_ERR_INVALID_ARG;
        }

        /* TODO: Open SD file and parse OBV1 */
        ESP_LOGW(TAG, "SD video source not fully implemented");
        return ESP_ERR_NOT_SUPPORTED;
    } else {
        /* Load from bootmedia partition */
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bootmedia");

        if (!part) {
            ESP_LOGE(TAG, "Bootmedia partition not found");
            return ESP_ERR_NOT_FOUND;
        }

        /* TODO: mmap partition and parse OBV1 */
        ESP_LOGW(TAG, "Flash video source not fully implemented");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Allocate framebuffers for composite mode */
    if (config->mode == VIDEO_MODE_COMPOSITE) {
        for (int i = 0; i < 2; i++) {
            s_player.fb[i] = heap_caps_malloc(FRAMEBUFFER_SIZE,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_player.fb[i]) {
                ESP_LOGE(TAG, "Failed to allocate framebuffer %d", i);
                return ESP_ERR_NO_MEM;
            }
        }
        s_player.fb_index = 0;
    }

    /* Create tasks */
    xTaskCreatePinnedToCore(read_task_func, "video_read", 4096, NULL, 3,
                            &s_player.read_task, 0);
    xTaskCreatePinnedToCore(decode_task_func, "video_decode", 8192, NULL, 5,
                            &s_player.decode_task, 1);

    s_player.state = VIDEO_STATE_PLAYING;
    ESP_LOGI(TAG, "Playback started");
    return ESP_OK;
}

esp_err_t video_player_stop(void) {
    if (s_player.state == VIDEO_STATE_STOPPED) {
        return ESP_OK;
    }

    s_player.stop_requested = true;

    /* Wait for tasks to exit */
    if (s_player.read_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_player.read_task = NULL;
    }

    if (s_player.decode_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
        s_player.decode_task = NULL;
    }

    /* Free framebuffers */
    for (int i = 0; i < 2; i++) {
        if (s_player.fb[i]) {
            heap_caps_free(s_player.fb[i]);
            s_player.fb[i] = NULL;
        }
    }

    /* Close video */
    obv1_close(&s_player.obv1);

    /* Reset ring buffer */
    s_player.ring_write_pos = 0;
    s_player.ring_read_pos = 0;
    s_player.ring_bytes_used = 0;

    s_player.state = VIDEO_STATE_STOPPED;
    ESP_LOGI(TAG, "Playback stopped");
    return ESP_OK;
}

esp_err_t video_player_pause(void) {
    if (s_player.state == VIDEO_STATE_PLAYING) {
        s_player.state = VIDEO_STATE_PAUSED;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t video_player_resume(void) {
    if (s_player.state == VIDEO_STATE_PAUSED) {
        s_player.state = VIDEO_STATE_PLAYING;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

video_state_t video_player_get_state(void) {
    return s_player.state;
}

void video_player_get_stats(video_stats_t *stats) {
    if (stats) {
        *stats = s_player.stats;
    }
}

esp_err_t video_player_seek(uint32_t frame_idx) {
    /* TODO: Implement seek
     * 1. Clear ring buffer
     * 2. Set current_frame
     * 3. Resume playback from new position
     */
    return ESP_ERR_NOT_SUPPORTED;
}
