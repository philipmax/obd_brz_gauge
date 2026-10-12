#pragma once
/*
 * ble_hub.h — BLE dispatcher for multiple services
 *
 * Coordinates:
 *   - ELM327 client (primary OBD connection)
 *   - RaceChrono DIY peripheral
 *   - ESP-NOW peer management
 *   - Future: BLE OTA (if re-implemented)
 *
 * Prevents conflicts when multiple BLE roles need to coexist.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BLE service types */
typedef enum {
    BLE_SERVICE_NONE = 0,
    BLE_SERVICE_ELM327,        /* ELM327 GATT client */
    BLE_SERVICE_RACECHRONO,    /* RaceChrono DIY server */
    BLE_SERVICE_ESPNOW,        /* ESP-NOW coordination */
} ble_service_t;

/* BLE hub state */
typedef enum {
    BLE_HUB_STATE_OFF = 0,
    BLE_HUB_STATE_INIT,
    BLE_HUB_STATE_READY,
    BLE_HUB_STATE_ERROR,
} ble_hub_state_t;

/**
 * Initialize BLE hub (does not start any services).
 */
esp_err_t ble_hub_init(void);

/**
 * Request exclusive access to BLE for a service.
 * Returns ESP_OK if granted, ESP_ERR_INVALID_STATE if another service is active.
 */
esp_err_t ble_hub_request_service(ble_service_t service);

/**
 * Release service (allows other services to request).
 */
esp_err_t ble_hub_release_service(ble_service_t service);

/**
 * Get active service.
 */
ble_service_t ble_hub_get_active_service(void);

/**
 * Get hub state.
 */
ble_hub_state_t ble_hub_get_state(void);

/**
 * Check if service is active.
 */
bool ble_hub_is_service_active(ble_service_t service);

/**
 * Shutdown BLE hub (stops all services).
 */
esp_err_t ble_hub_shutdown(void);

#ifdef __cplusplus
}
#endif
