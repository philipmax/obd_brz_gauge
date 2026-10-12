/*
 * ble_hub.c — BLE dispatcher for multiple services
 */

#include "ble_hub.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ble_hub";

static ble_hub_state_t s_state = BLE_HUB_STATE_OFF;
static ble_service_t s_active_service = BLE_SERVICE_NONE;
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t ble_hub_init(void) {
    if (s_mutex) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* Initialize Bluetooth controller */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %d", err);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_state = BLE_HUB_STATE_ERROR;
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %d", err);
        esp_bt_controller_deinit();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_state = BLE_HUB_STATE_ERROR;
        return err;
    }

    /* Initialize Bluedroid stack */
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %d", err);
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_state = BLE_HUB_STATE_ERROR;
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %d", err);
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_state = BLE_HUB_STATE_ERROR;
        return err;
    }

    s_state = BLE_HUB_STATE_READY;
    s_active_service = BLE_SERVICE_NONE;

    ESP_LOGI(TAG, "BLE hub initialized");
    return ESP_OK;
}

esp_err_t ble_hub_request_service(ble_service_t service) {
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (service == BLE_SERVICE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_state != BLE_HUB_STATE_READY) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Check if another service is active */
    if (s_active_service != BLE_SERVICE_NONE && s_active_service != service) {
        ESP_LOGW(TAG, "Service %d already active, cannot activate %d", s_active_service, service);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Grant access */
    s_active_service = service;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Service %d activated", service);
    return ESP_OK;
}

esp_err_t ble_hub_release_service(ble_service_t service) {
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_active_service != service) {
        ESP_LOGW(TAG, "Service %d not active, cannot release", service);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_active_service = BLE_SERVICE_NONE;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Service %d released", service);
    return ESP_OK;
}

ble_service_t ble_hub_get_active_service(void) {
    if (!s_mutex) {
        return BLE_SERVICE_NONE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ble_service_t service = s_active_service;
    xSemaphoreGive(s_mutex);

    return service;
}

ble_hub_state_t ble_hub_get_state(void) {
    return s_state;
}

bool ble_hub_is_service_active(ble_service_t service) {
    return ble_hub_get_active_service() == service;
}

esp_err_t ble_hub_shutdown(void) {
    if (!s_mutex) {
        return ESP_OK;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Release active service */
    s_active_service = BLE_SERVICE_NONE;

    /* Disable Bluedroid */
    esp_bluedroid_disable();
    esp_bluedroid_deinit();

    /* Disable controller */
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    s_state = BLE_HUB_STATE_OFF;

    xSemaphoreGive(s_mutex);
    vSemaphoreDelete(s_mutex);
    s_mutex = NULL;

    ESP_LOGI(TAG, "BLE hub shutdown");
    return ESP_OK;
}
