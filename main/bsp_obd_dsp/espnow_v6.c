/*
 * espnow_v6.c — ESP-NOW v6 protocol implementation
 */

#include "espnow_v6.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "espnow_v6";

static espnow_v6_recv_cb_t s_recv_cb = NULL;
static espnow_v6_stats_t s_stats = {0};
static SemaphoreHandle_t s_stats_mutex = NULL;

/* CRC8 calculation (polynomial 0x07) */
uint8_t espnow_v6_calc_crc8(const espnow_v6_data_t *data) {
    const uint8_t *ptr = (const uint8_t *)data;
    size_t len = sizeof(espnow_v6_data_t) - 1; /* Exclude crc8 field itself */

    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= ptr[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ESP-NOW send callback */
static void espnow_send_cb(const uint8_t *mac, esp_now_send_status_t status) {
    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
        if (status == ESP_NOW_SEND_SUCCESS) {
            /* Success counted in broadcast function */
        } else {
            s_stats.tx_errors++;
        }
        xSemaphoreGive(s_stats_mutex);
    }
}

/* ESP-NOW receive callback */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(espnow_v6_data_t)) {
        ESP_LOGW(TAG, "Invalid packet size: %d", len);
        return;
    }

    espnow_v6_data_t *pkt = (espnow_v6_data_t *)data;

    /* Verify version */
    if (pkt->version != ESPNOW_V6_PROTOCOL_VERSION) {
        if (s_stats_mutex) {
            xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
            s_stats.rx_version_errors++;
            xSemaphoreGive(s_stats_mutex);
        }
        ESP_LOGW(TAG, "Version mismatch: got %d, expected %d", pkt->version, ESPNOW_V6_PROTOCOL_VERSION);
        return;
    }

    /* Verify CRC */
    uint8_t calc_crc = espnow_v6_calc_crc8(pkt);
    if (calc_crc != pkt->crc8) {
        if (s_stats_mutex) {
            xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
            s_stats.rx_crc_errors++;
            xSemaphoreGive(s_stats_mutex);
        }
        ESP_LOGW(TAG, "CRC error: got 0x%02X, expected 0x%02X", pkt->crc8, calc_crc);
        return;
    }

    /* Update stats */
    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
        s_stats.rx_packets++;
        s_stats.rx_bytes += len;
        xSemaphoreGive(s_stats_mutex);
    }

    /* Call user callback */
    if (s_recv_cb) {
        s_recv_cb(pkt, info->src_addr);
    }
}

esp_err_t espnow_v6_init(void) {
    /* Initialize WiFi in STA mode (required for ESP-NOW) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG, "WiFi init failed: %d", err);
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed: %d", err);
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "WiFi start failed: %d", err);
        return err;
    }

    /* Initialize ESP-NOW */
    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed: %d", err);
        return err;
    }

    /* Register callbacks */
    err = esp_now_register_send_cb(espnow_send_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register send callback failed: %d", err);
        esp_now_deinit();
        return err;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Register recv callback failed: %d", err);
        esp_now_deinit();
        return err;
    }

    /* Create mutex */
    s_stats_mutex = xSemaphoreCreateMutex();
    if (!s_stats_mutex) {
        esp_now_deinit();
        return ESP_ERR_NO_MEM;
    }

    memset(&s_stats, 0, sizeof(s_stats));

    ESP_LOGI(TAG, "ESP-NOW v6 initialized");
    return ESP_OK;
}

esp_err_t espnow_v6_add_peer(const uint8_t mac[6]) {
    esp_now_peer_info_t peer = {
        .channel = 0,  /* Use current channel */
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, 6);

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "Add peer failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Peer added: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t espnow_v6_remove_peer(const uint8_t mac[6]) {
    esp_err_t err = esp_now_del_peer(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Remove peer failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Peer removed: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t espnow_v6_broadcast(const espnow_v6_data_t *data) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Make a copy and fill in CRC */
    espnow_v6_data_t pkt = *data;
    pkt.version = ESPNOW_V6_PROTOCOL_VERSION;
    pkt.crc8 = espnow_v6_calc_crc8(&pkt);

    /* Send to all paired peers */
    esp_err_t err = esp_now_send(NULL, (const uint8_t *)&pkt, sizeof(pkt));

    if (s_stats_mutex) {
        xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
        if (err == ESP_OK) {
            s_stats.tx_packets++;
            s_stats.tx_bytes += sizeof(pkt);
        } else {
            s_stats.tx_errors++;
        }
        xSemaphoreGive(s_stats_mutex);
    }

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Broadcast failed: %d", err);
    }

    return err;
}

esp_err_t espnow_v6_register_recv_callback(espnow_v6_recv_cb_t cb) {
    s_recv_cb = cb;
    return ESP_OK;
}

esp_err_t espnow_v6_get_stats(espnow_v6_stats_t *stats) {
    if (!stats || !s_stats_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    *stats = s_stats;
    xSemaphoreGive(s_stats_mutex);

    return ESP_OK;
}
