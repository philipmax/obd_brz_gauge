#pragma once
/*
 * lifecycle_state.h — unified device lifecycle state machine
 *
 * Replaces scattered state flags (s_ota_paused, s_connected, s_elm_ready, etc.)
 * with a single coherent state machine that coordinates:
 *   - Boot → Init → Ready
 *   - ELM327 connection/disconnection
 *   - OTA mode (WiFi/BLE exclusive access)
 *   - Showroom mode (UI demo data)
 *   - Import operations (theme/bootmedia from SD)
 *   - Sleep/wake
 *
 * Single source of truth for "what is the device doing right now?"
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device lifecycle states */
typedef enum {
    LIFECYCLE_BOOT = 0,           /* Power-on, not ready */
    LIFECYCLE_INIT,               /* Initializing peripherals */
    LIFECYCLE_READY,              /* Normal operation */
    LIFECYCLE_ELM_CONNECTING,     /* Scanning/connecting to ELM327 */
    LIFECYCLE_ELM_CONNECTED,      /* ELM327 connected, ready to poll */
    LIFECYCLE_OTA_WIFI,           /* WiFi OTA mode (BLE paused) */
    LIFECYCLE_OTA_BLE,            /* BLE OTA mode (deprecated, not implemented) */
    LIFECYCLE_IMPORT_SD,          /* Importing theme/bootmedia from SD */
    LIFECYCLE_SHOWROOM,           /* UI demo mode */
    LIFECYCLE_SLEEP,              /* Low power sleep */
    LIFECYCLE_ERROR,              /* Unrecoverable error */
} lifecycle_state_t;

/* State transition events */
typedef enum {
    LIFECYCLE_EVENT_BOOT_DONE = 0,
    LIFECYCLE_EVENT_INIT_DONE,
    LIFECYCLE_EVENT_ELM_SCAN_START,
    LIFECYCLE_EVENT_ELM_CONNECTED,
    LIFECYCLE_EVENT_ELM_DISCONNECTED,
    LIFECYCLE_EVENT_OTA_WIFI_START,
    LIFECYCLE_EVENT_OTA_WIFI_END,
    LIFECYCLE_EVENT_IMPORT_START,
    LIFECYCLE_EVENT_IMPORT_END,
    LIFECYCLE_EVENT_SHOWROOM_START,
    LIFECYCLE_EVENT_SHOWROOM_END,
    LIFECYCLE_EVENT_SLEEP,
    LIFECYCLE_EVENT_WAKE,
    LIFECYCLE_EVENT_ERROR,
} lifecycle_event_t;

/* State change callback */
typedef void (*lifecycle_cb_t)(lifecycle_state_t old_state, lifecycle_state_t new_state);

/**
 * Initialize lifecycle state machine.
 */
esp_err_t lifecycle_init(void);

/**
 * Get current state.
 */
lifecycle_state_t lifecycle_get_state(void);

/**
 * Post an event to the state machine.
 * Returns ESP_OK if transition is valid, ESP_ERR_INVALID_STATE if not.
 */
esp_err_t lifecycle_post_event(lifecycle_event_t event);

/**
 * Register state change callback.
 */
esp_err_t lifecycle_register_callback(lifecycle_cb_t cb);

/**
 * Query helpers (avoid scattered state checks).
 */
bool lifecycle_is_elm_ready(void);         /* ELM327 connected and ready */
bool lifecycle_is_ota_active(void);        /* Any OTA mode active */
bool lifecycle_can_poll_obd(void);         /* Safe to send OBD requests */
bool lifecycle_can_use_ble(void);          /* BLE radio available */
bool lifecycle_can_use_wifi(void);         /* WiFi radio available */

/**
 * Get state name string (for logging).
 */
const char *lifecycle_state_name(lifecycle_state_t state);

#ifdef __cplusplus
}
#endif
