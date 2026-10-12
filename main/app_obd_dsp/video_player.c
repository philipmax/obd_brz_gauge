/*
 * Video Player Core
 * Ring buffer + state machine for OBV1 playback
 */

#include "video_player.h"
#include "obv_parser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "video_player";

#define RING_BUFFER_SIZE (1280 * 1024)  // 1.25MB default
#define DMA_BUFFER_SIZE (16 * 1024)     // 16KB internal DMA buffer
#define FILL_WATERMARK (512 * 1024)     // Start playback at 0.5s buffered

typedef enum {
    VP_STATE_IDLE,
    VP_STATE_LOADING,
    VP_STATE_PLAYING,
    VP_STATE_PAUSED,
    VP_STATE_STOPPED
} vp_state_t;

typedef struct {
    obv_ctx_t *obv_ctx;
    uint8_t *ring_buffer;        // PSRAM ring buffer
    uint8_t *dma_buffer;         // Internal RAM DMA buffer
    size_t ring_size;
    size_t ring_head;
    size_t ring_tail;
    uint32_t current_frame;
    vp_state_t state;
    TaskHandle_t read_task;
    SemaphoreHandle_t mutex;
    video_player_callbacks_t callbacks;
} video_player_ctx_t;

static video_player_ctx_t s_player = {0};

static void read_task(void *arg);

esp_err_t video_player_init(void) {
    if (s_player.ring_buffer) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Allocate PSRAM ring buffer
    s_player.ring_buffer = heap_caps_malloc(RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_player.ring_buffer) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer");
        return ESP_ERR_NO_MEM;
    }

    // Allocate internal DMA buffer
    s_player.dma_buffer = heap_caps_malloc(DMA_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_player.dma_buffer) {
        free(s_player.ring_buffer);
        s_player.ring_buffer = NULL;
        ESP_LOGE(TAG, "Failed to allocate DMA buffer");
        return ESP_ERR_NO_MEM;
    }

    s_player.ring_size = RING_BUFFER_SIZE;
    s_player.mutex = xSemaphoreCreateMutex();
    if (!s_player.mutex) {
        free(s_player.dma_buffer);
        free(s_player.ring_buffer);
        s_player.ring_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_player.state = VP_STATE_IDLE;
    ESP_LOGI(TAG, "Video player initialized");

    return ESP_OK;
}

esp_err_t video_player_deinit(void) {
    video_player_stop();

    if (s_player.mutex) {
        vSemaphoreDelete(s_player.mutex);
        s_player.mutex = NULL;
    }

    if (s_player.ring_buffer) {
        free(s_player.ring_buffer);
        s_player.ring_buffer = NULL;
    }

    if (s_player.dma_buffer) {
        free(s_player.dma_buffer);
        s_player.dma_buffer = NULL;
    }

    return ESP_OK;
}

esp_err_t video_player_open(const char *path) {
    if (s_player.state != VP_STATE_IDLE) {
        ESP_LOGE(TAG, "Player not idle");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = obv_open(path, &s_player.obv_ctx);
    if (ret != ESP_OK) {
        return ret;
    }

    s_player.ring_head = 0;
    s_player.ring_tail = 0;
    s_player.current_frame = 0;
    s_player.state = VP_STATE_LOADING;

    // Start read task
    xTaskCreatePinnedToCore(read_task, "video_read", 4096, NULL, 3, &s_player.read_task, 0);

    ESP_LOGI(TAG, "Opened video: %dx%d, %d frames, %d fps",
             obv_get_width(s_player.obv_ctx),
             obv_get_height(s_player.obv_ctx),
             obv_get_frame_count(s_player.obv_ctx),
             obv_get_fps(s_player.obv_ctx));

    return ESP_OK;
}

esp_err_t video_player_play(void) {
    xSemaphoreTake(s_player.mutex, portMAX_DELAY);
    if (s_player.state == VP_STATE_LOADING || s_player.state == VP_STATE_PAUSED) {
        s_player.state = VP_STATE_PLAYING;
    }
    xSemaphoreGive(s_player.mutex);
    return ESP_OK;
}

esp_err_t video_player_pause(void) {
    xSemaphoreTake(s_player.mutex, portMAX_DELAY);
    if (s_player.state == VP_STATE_PLAYING) {
        s_player.state = VP_STATE_PAUSED;
    }
    xSemaphoreGive(s_player.mutex);
    return ESP_OK;
}

esp_err_t video_player_stop(void) {
    xSemaphoreTake(s_player.mutex, portMAX_DELAY);
    vp_state_t prev_state = s_player.state;
    s_player.state = VP_STATE_STOPPED;
    xSemaphoreGive(s_player.mutex);

    if (prev_state != VP_STATE_IDLE && s_player.read_task) {
        // Wait for read task to exit
        vTaskDelay(pdMS_TO_TICKS(100));
        s_player.read_task = NULL;
    }

    if (s_player.obv_ctx) {
        obv_close(s_player.obv_ctx);
        s_player.obv_ctx = NULL;
    }

    s_player.state = VP_STATE_IDLE;
    return ESP_OK;
}

void video_player_set_callbacks(video_player_callbacks_t *callbacks) {
    if (callbacks) {
        memcpy(&s_player.callbacks, callbacks, sizeof(video_player_callbacks_t));
    } else {
        memset(&s_player.callbacks, 0, sizeof(video_player_callbacks_t));
    }
}

video_player_state_t video_player_get_state(void) {
    return (video_player_state_t)s_player.state;
}

static void read_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "Read task started");

    while (s_player.state != VP_STATE_STOPPED) {
        // TODO: Implement ring buffer filling logic
        // Read frames from OBV, fill ring buffer, maintain watermark

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Read task exiting");
    vTaskDelete(NULL);
}
