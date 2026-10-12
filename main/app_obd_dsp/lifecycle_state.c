/*
 * lifecycle_state.c — unified device lifecycle state machine
 */

#include "lifecycle_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "lifecycle";

static lifecycle_state_t s_current_state = LIFECYCLE_BOOT;
static SemaphoreHandle_t s_state_mutex = NULL;

#define MAX_CALLBACKS 8
static lifecycle_cb_t s_callbacks[MAX_CALLBACKS];
static uint8_t s_callback_count = 0;

/* State transition table: [from_state][event] = to_state (0xFF = invalid) */
static const uint8_t s_transition_table[12][14] = {
    /* BOOT */ {
        [LIFECYCLE_EVENT_BOOT_DONE] = LIFECYCLE_INIT,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* INIT */ {
        [LIFECYCLE_EVENT_INIT_DONE] = LIFECYCLE_READY,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* READY */ {
        [LIFECYCLE_EVENT_ELM_SCAN_START] = LIFECYCLE_ELM_CONNECTING,
        [LIFECYCLE_EVENT_ELM_CONNECTED] = LIFECYCLE_ELM_CONNECTED,
        [LIFECYCLE_EVENT_OTA_WIFI_START] = LIFECYCLE_OTA_WIFI,
        [LIFECYCLE_EVENT_IMPORT_START] = LIFECYCLE_IMPORT_SD,
        [LIFECYCLE_EVENT_SHOWROOM_START] = LIFECYCLE_SHOWROOM,
        [LIFECYCLE_EVENT_SLEEP] = LIFECYCLE_SLEEP,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* ELM_CONNECTING */ {
        [LIFECYCLE_EVENT_ELM_CONNECTED] = LIFECYCLE_ELM_CONNECTED,
        [LIFECYCLE_EVENT_ELM_DISCONNECTED] = LIFECYCLE_READY,
        [LIFECYCLE_EVENT_OTA_WIFI_START] = LIFECYCLE_OTA_WIFI,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* ELM_CONNECTED */ {
        [LIFECYCLE_EVENT_ELM_DISCONNECTED] = LIFECYCLE_READY,
        [LIFECYCLE_EVENT_OTA_WIFI_START] = LIFECYCLE_OTA_WIFI,
        [LIFECYCLE_EVENT_SHOWROOM_START] = LIFECYCLE_SHOWROOM,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* OTA_WIFI */ {
        [LIFECYCLE_EVENT_OTA_WIFI_END] = LIFECYCLE_READY,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* OTA_BLE */ {
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* IMPORT_SD */ {
        [LIFECYCLE_EVENT_IMPORT_END] = LIFECYCLE_READY,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* SHOWROOM */ {
        [LIFECYCLE_EVENT_SHOWROOM_END] = LIFECYCLE_READY,
        [LIFECYCLE_EVENT_ELM_CONNECTED] = LIFECYCLE_ELM_CONNECTED,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* SLEEP */ {
        [LIFECYCLE_EVENT_WAKE] = LIFECYCLE_READY,
        [LIFECYCLE_EVENT_ERROR] = LIFECYCLE_ERROR,
    },
    /* ERROR */ {
        [LIFECYCLE_EVENT_INIT_DONE] = LIFECYCLE_READY,
    },
};

esp_err_t lifecycle_init(void) {
    if (s_state_mutex) {
        return ESP_OK; /* Already initialized */
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_current_state = LIFECYCLE_BOOT;
    s_callback_count = 0;
    memset(s_callbacks, 0, sizeof(s_callbacks));

    ESP_LOGI(TAG, "Lifecycle state machine initialized");
    return ESP_OK;
}

lifecycle_state_t lifecycle_get_state(void) {
    if (!s_state_mutex) {
        return LIFECYCLE_BOOT;
    }

    lifecycle_state_t state;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    state = s_current_state;
    xSemaphoreGive(s_state_mutex);

    return state;
}

esp_err_t lifecycle_post_event(lifecycle_event_t event) {
    if (!s_state_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    lifecycle_state_t old_state = s_current_state;

    /* Check transition table */
    if (old_state >= 12 || event >= 14) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t new_state = s_transition_table[old_state][event];
    if (new_state == 0 && event != 0) {
        /* Invalid transition */
        ESP_LOGW(TAG, "Invalid transition: %s + event %d",
                 lifecycle_state_name(old_state), event);
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (new_state == old_state) {
        xSemaphoreGive(s_state_mutex);
        return ESP_OK; /* No-op transition */
    }

    /* Perform transition */
    s_current_state = (lifecycle_state_t)new_state;

    ESP_LOGI(TAG, "State: %s -> %s",
             lifecycle_state_name(old_state),
             lifecycle_state_name(s_current_state));

    /* Notify callbacks */
    lifecycle_cb_t cbs[MAX_CALLBACKS];
    uint8_t cb_count = s_callback_count;
    memcpy(cbs, s_callbacks, sizeof(cbs));

    xSemaphoreGive(s_state_mutex);

    /* Call callbacks outside mutex */
    for (uint8_t i = 0; i < cb_count; i++) {
        if (cbs[i]) {
            cbs[i](old_state, s_current_state);
        }
    }

    return ESP_OK;
}

esp_err_t lifecycle_register_callback(lifecycle_cb_t cb) {
    if (!s_state_mutex || !cb) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (s_callback_count >= MAX_CALLBACKS) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_callbacks[s_callback_count++] = cb;

    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

bool lifecycle_is_elm_ready(void) {
    lifecycle_state_t state = lifecycle_get_state();
    return (state == LIFECYCLE_ELM_CONNECTED);
}

bool lifecycle_is_ota_active(void) {
    lifecycle_state_t state = lifecycle_get_state();
    return (state == LIFECYCLE_OTA_WIFI || state == LIFECYCLE_OTA_BLE);
}

bool lifecycle_can_poll_obd(void) {
    lifecycle_state_t state = lifecycle_get_state();
    /* Can poll when ELM connected and not in OTA/import/showroom */
    return (state == LIFECYCLE_ELM_CONNECTED);
}

bool lifecycle_can_use_ble(void) {
    lifecycle_state_t state = lifecycle_get_state();
    /* BLE available unless in WiFi OTA (radio conflict) */
    return (state != LIFECYCLE_OTA_WIFI);
}

bool lifecycle_can_use_wifi(void) {
    lifecycle_state_t state = lifecycle_get_state();
    /* WiFi available when explicitly requested or in OTA mode */
    return (state == LIFECYCLE_OTA_WIFI || state == LIFECYCLE_IMPORT_SD);
}

const char *lifecycle_state_name(lifecycle_state_t state) {
    switch (state) {
        case LIFECYCLE_BOOT: return "BOOT";
        case LIFECYCLE_INIT: return "INIT";
        case LIFECYCLE_READY: return "READY";
        case LIFECYCLE_ELM_CONNECTING: return "ELM_CONNECTING";
        case LIFECYCLE_ELM_CONNECTED: return "ELM_CONNECTED";
        case LIFECYCLE_OTA_WIFI: return "OTA_WIFI";
        case LIFECYCLE_OTA_BLE: return "OTA_BLE";
        case LIFECYCLE_IMPORT_SD: return "IMPORT_SD";
        case LIFECYCLE_SHOWROOM: return "SHOWROOM";
        case LIFECYCLE_SLEEP: return "SLEEP";
        case LIFECYCLE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
