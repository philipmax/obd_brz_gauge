// Triple-gauge BLE pairing: one-shot BLE central on the slave gauge side — scans for nearby
// "SkyGauge" master gauge advertising, connects, reads its pairing characteristic (this
// device's ESP-NOW MAC), and disconnects immediately after reading. No persistent
// notification subscription, no auto-reconnect on disconnect (this is a user-initiated
// one-shot pairing action, not a long-lived connection).
//
// Registers itself with ble_hub as GATTC app BLE_HUB_APP_GAUGE_PAIR (the hub owns the
// Bluedroid callbacks and the stack lifecycle, and arbitrates the scanner). The hub hands
// every module the GATTC CONNECT / DISCONNECT events of every LE link, so the link is
// tracked by OPEN_EVT (ours only) and the target address.

#include "gauge_pair_ble_client.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp_obd_dsp/ble_adv_util.h"
#include "bsp_obd_dsp/ble_hub.h"

static const char *TAG = "gauge_pair_ble";

#define GAUGE_PAIR_NAME_PREFIX "SkyGauge"
#define GAUGE_PAIR_NAME_PREFIX_LEN (sizeof(GAUGE_PAIR_NAME_PREFIX) - 1)
#define GAUGE_PAIR_TIMEOUT_US  (8 * 1000 * 1000)
#define GAUGE_PAIR_FAIL_NOW_US 1000   // immediate failure: still reported from the timer task, never inline

static bool s_registered = false;

// ---- Scan state ----
static bool s_scanning = false;
static gauge_pair_scan_cb_t s_scan_cb = NULL;
static gauge_pair_scan_result_t s_scan_list[GAUGE_PAIR_SCAN_MAX_DEVICES];
static int s_scan_count = 0;

// ---- Connection/pairing state ----
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0xFFFF;
static bool s_gattc_connected = false;
static bool s_pairing = false;               // a pairing flow is in progress
static uint8_t s_target_bda[6] = {0};
static char s_target_name[32] = {0};
static bool s_have_pair_svc = false;
static uint16_t s_pair_svc_start = 0, s_pair_svc_end = 0;
static uint16_t s_char_mac_handle = 0;
static gauge_pair_result_cb_t s_pair_cb = NULL;
static esp_timer_handle_t s_timeout_timer = NULL;

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void report_result(bool ok, const uint8_t mac[6]);

static void close_link(void) {
    if (s_gattc_connected && s_gattc_if != ESP_GATT_IF_NONE && ble_hub_is_ready()) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
    }
}

static void pair_timeout_cb(void *arg) {
    (void)arg;
    if (!s_pairing) return;
    ESP_LOGW(TAG, "Pairing timeout / failed");
    close_link();
    report_result(false, NULL);
}

static bool ensure_timeout_timer(void) {
    if (s_timeout_timer) return true;
    const esp_timer_create_args_t args = {
        .callback = pair_timeout_cb,
        .name = "gauge_pair_to",
    };
    return esp_timer_create(&args, &s_timeout_timer) == ESP_OK;
}

static void report_result(bool ok, const uint8_t mac[6]) {
    if (!s_pairing) return;   // already reported once; ignore later calls (e.g. DISCONNECT right after a successful READ)
    s_pairing = false;
    if (s_timeout_timer) esp_timer_stop(s_timeout_timer);

    gauge_pair_result_cb_t cb = s_pair_cb;
    s_pair_cb = NULL;
    char name_copy[32];
    strncpy(name_copy, s_target_name, sizeof(name_copy) - 1);
    name_copy[sizeof(name_copy) - 1] = '\0';

    if (cb) cb(ok, ok ? name_copy : NULL, ok ? mac : NULL);
}

static bool ensure_registered(void) {
    if (s_registered) return true;
    const ble_hub_client_t client = {
        .gap_cb = gap_cb,
        .gattc_cb = gattc_cb,
        .gatts_cb = NULL,
    };
    esp_err_t err = ble_hub_register(BLE_HUB_APP_GAUGE_PAIR, &client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ble_hub register failed: %s", esp_err_to_name(err));
        return false;
    }
    s_registered = true;
    return true;
}

// Scan results reach this module only while it owns the hub scanner.
static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT || !s_scanning) return;

    esp_ble_gap_cb_param_t *pr = param;
    if (pr->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        s_scanning = false;   // scan window over (the hub has released the scanner)
        return;
    }
    if (pr->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;

    char dev_name[32] = {0};
    ble_adv_extract_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len,
                      pr->scan_rst.scan_rsp_len, dev_name, sizeof(dev_name));
    if (strncmp(dev_name, GAUGE_PAIR_NAME_PREFIX, GAUGE_PAIR_NAME_PREFIX_LEN) != 0) return;
    if (s_scan_count >= GAUGE_PAIR_SCAN_MAX_DEVICES) return;

    for (int i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_list[i].name, dev_name) == 0) return;
    }

    strncpy(s_scan_list[s_scan_count].name, dev_name, sizeof(s_scan_list[s_scan_count].name) - 1);
    memcpy(s_scan_list[s_scan_count].addr, pr->scan_rst.bda, 6);
    s_scan_list[s_scan_count].rssi = pr->scan_rst.rssi;
    s_scan_count++;
    ESP_LOGI(TAG, "Found master [%d]: %s (RSSI %d)", s_scan_count, dev_name, pr->scan_rst.rssi);
    if (s_scan_cb) s_scan_cb(&s_scan_list[s_scan_count - 1], s_scan_count);
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_REG_EVT:
        // ble_hub delivers REG_EVT by app id: this is ours
        s_gattc_if = (param->reg.status == ESP_GATT_OK) ? gattc_if : ESP_GATT_IF_NONE;
        break;

    case ESP_GATTC_OPEN_EVT:
        if (memcmp(param->open.remote_bda, s_target_bda, 6) != 0) break;
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "Open failed status=%d", param->open.status);
            report_result(false, NULL);
            break;
        }
        s_gattc_connected = true;
        s_conn_id = param->open.conn_id;
        if (!s_pairing) {
            // pairing already timed out while the open was pending
            close_link();
            break;
        }
        // Enumerate all services without a filter and compare UUIDs ourselves in SEARCH_RES_EVT
        // (same service discovery approach as elm327_ble_client.c, avoiding reliance on UUID-filtered search behavior).
        esp_ble_gattc_search_service(gattc_if, s_conn_id, NULL);
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        if (!s_gattc_connected || param->search_res.conn_id != s_conn_id) break;
        const esp_gatt_id_t *srvc_id = &param->search_res.srvc_id;
        if (srvc_id->uuid.len == ESP_UUID_LEN_16 && srvc_id->uuid.uuid.uuid16 == GAUGE_PAIR_SERVICE_UUID) {
            s_have_pair_svc = true;
            s_pair_svc_start = param->search_res.start_handle;
            s_pair_svc_end = param->search_res.end_handle;
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (!s_gattc_connected || param->search_cmpl.conn_id != s_conn_id) break;
        if (!s_have_pair_svc) {
            ESP_LOGW(TAG, "Pair service not found on peer");
            report_result(false, NULL);
            close_link();
            break;
        }

        uint16_t char_count = 0;
        esp_gatt_status_t ret = esp_ble_gattc_get_attr_count(gattc_if, s_conn_id, ESP_GATT_DB_CHARACTERISTIC,
                                                             s_pair_svc_start, s_pair_svc_end, 0, &char_count);
        if (ret != ESP_GATT_OK || char_count == 0) {
            ESP_LOGW(TAG, "No characteristics in pair service");
            report_result(false, NULL);
            close_link();
            break;
        }

        esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t *)malloc(char_count * sizeof(esp_gattc_char_elem_t));
        if (!chars) {
            report_result(false, NULL);
            close_link();
            break;
        }
        ret = esp_ble_gattc_get_all_char(gattc_if, s_conn_id, s_pair_svc_start, s_pair_svc_end,
                                         chars, &char_count, 0);
        s_char_mac_handle = 0;
        if (ret == ESP_GATT_OK) {
            for (int i = 0; i < char_count; i++) {
                if (chars[i].uuid.len == ESP_UUID_LEN_16 && chars[i].uuid.uuid.uuid16 == GAUGE_PAIR_CHAR_MAC) {
                    s_char_mac_handle = chars[i].char_handle;
                    break;
                }
            }
        }
        free(chars);

        if (s_char_mac_handle == 0) {
            ESP_LOGW(TAG, "Pair MAC characteristic not found");
            report_result(false, NULL);
            close_link();
            break;
        }

        esp_ble_gattc_read_char(gattc_if, s_conn_id, s_char_mac_handle, ESP_GATT_AUTH_REQ_NONE);
        break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
        if (!s_gattc_connected || param->read.conn_id != s_conn_id) break;
        if (param->read.status == ESP_GATT_OK && param->read.handle == s_char_mac_handle &&
            param->read.value_len >= 6) {
            report_result(true, param->read.value);
        } else {
            ESP_LOGW(TAG, "Read pair MAC failed status=%d len=%d", param->read.status, param->read.value_len);
            report_result(false, NULL);
        }
        close_link();
        break;
    }

    case ESP_GATTC_DISCONNECT_EVT:
        // Every LE link's drop reaches every GATTC app: only ours counts.
        if (!s_gattc_connected || param->disconnect.conn_id != s_conn_id ||
            memcmp(param->disconnect.remote_bda, s_target_bda, 6) != 0) {
            break;
        }
        s_gattc_connected = false;
        s_conn_id = 0xFFFF;
        s_have_pair_svc = false;
        s_pair_svc_start = s_pair_svc_end = 0;
        s_char_mac_handle = 0;
        report_result(false, NULL);   // if already reported in READ_CHAR_EVT, report_result simply ignores this
        break;

    default:
        break;
    }
}

void gauge_pair_ble_scan_start(int duration_s, gauge_pair_scan_cb_t cb) {
    if (!ensure_registered()) return;
    s_scan_cb = cb;
    s_scan_count = 0;
    memset(s_scan_list, 0, sizeof(s_scan_list));
    s_scanning = true;

    const esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x60,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    esp_err_t err = ble_hub_scan_start(BLE_HUB_APP_GAUGE_PAIR, &scan_params,
                                       duration_s > 0 ? (uint32_t)duration_s : 0);
    if (err != ESP_OK) {
        s_scanning = false;
        ESP_LOGW(TAG, "Scan start failed: %s", esp_err_to_name(err));
    }
}

void gauge_pair_ble_scan_stop(void) {
    s_scanning = false;
    ble_hub_scan_stop(BLE_HUB_APP_GAUGE_PAIR);   // INVALID_STATE when not the owner: nothing to stop
}

void gauge_pair_ble_connect(const uint8_t addr[6], const char *name, gauge_pair_result_cb_t cb) {
    if (!addr || s_pairing) return;
    if (!ensure_registered() || !ensure_timeout_timer()) return;
    gauge_pair_ble_scan_stop();

    s_pairing = true;
    s_pair_cb = cb;
    memcpy(s_target_bda, addr, 6);
    if (name) {
        strncpy(s_target_name, name, sizeof(s_target_name) - 1);
        s_target_name[sizeof(s_target_name) - 1] = '\0';
    } else {
        s_target_name[0] = '\0';
    }
    s_have_pair_svc = false;
    s_pair_svc_start = s_pair_svc_end = 0;
    s_char_mac_handle = 0;

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_gattc_if != ESP_GATT_IF_NONE && ble_hub_is_ready()) {
        err = esp_ble_gattc_open(s_gattc_if, s_target_bda, BLE_ADDR_TYPE_PUBLIC, true);
    }
    if (err != ESP_OK) {
        // The caller's result callback takes the LVGL lock: report from the timer task, not inline.
        ESP_LOGW(TAG, "Open request failed: %s", esp_err_to_name(err));
        esp_timer_start_once(s_timeout_timer, GAUGE_PAIR_FAIL_NOW_US);
        return;
    }
    esp_timer_start_once(s_timeout_timer, GAUGE_PAIR_TIMEOUT_US);
}
