#include "racechrono_ble_diy.h"
#include "racechrono_filter.h"
#include "app_obd_dsp/perf_stats.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_mac.h"

#include "app_obd_dsp/signal_store.h"
#include "app_tasks.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/gauge_pair_ble_client.h"   // GAUGE_PAIR_SERVICE_UUID / GAUGE_PAIR_CHAR_MAC (shared with the slave-gauge pairing client)

#define RC_TAG "racechrono_diy"

#define RC_DEVICE_NAME "SkyGarageRC"

#define RC_SERVICE_UUID  0x1FF8
#define RC_CHAR_CAN_MAIN 0x0001
#define RC_CHAR_FILTER   0x0002

#define RC_CH_MAX RC_FILTER_CHANNEL_COUNT

typedef struct {
    uint32_t pid;
    int      slot;       /* SIG_SLOT_NONE for retired channels */
    int32_t  scale;
} rc_chan_t;

static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static esp_bd_addr_t s_peer_bda;
static bool s_connected = false;
static bool s_notify_enabled = false;
static bool s_started = false;
static bool s_rc_enabled = true;          // when false, skip RC (0x1FF8); only a MASTER publishes Pair (0x1FF9)
static bool s_adv_config_done = false;
static bool s_attr_ready = false;
static bool s_adv_start_pending = false;
static bool s_adv_cfg_pending = false;
static bool s_adv_raw_done = false;
static bool s_scan_rsp_raw_done = false;

static rc_filter_t s_filter = RC_FILTER_INITIALIZER;
static portMUX_TYPE s_filter_mux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t s_handle_can_main = 0;
static uint16_t s_handle_can_cccd = 0;
static uint16_t s_handle_filter = 0;
static int64_t s_last_send_err_log_us = 0;

static TaskHandle_t s_stream_task = NULL;
/* Lifetime is the entire boot, not the worker lifetime. Late GATT/shutdown
 * callbacks can safely give this token even after the worker deletes itself. */
static StaticSemaphore_t s_stream_wake_storage;
static SemaphoreHandle_t s_stream_wake;

static void wake_stream(void)
{
    if (s_stream_wake) xSemaphoreGive(s_stream_wake);
}

static TickType_t stream_wait_ticks(uint32_t wait_us)
{
    if (wait_us == RC_FILTER_WAIT_FOREVER) return portMAX_DELAY;
    uint64_t ticks = ((uint64_t)wait_us * configTICK_RATE_HZ + 999999u) / 1000000u;
    if (ticks >= portMAX_DELAY) ticks = portMAX_DELAY - 1u;
    return (TickType_t)(ticks ? ticks : 1u);
}

// Stable virtual CAN IDs: keep backward compatibility for RaceChrono formulas.
#define RC_PID_RPM          0x0000F101u
#define RC_PID_SPEED_KMH    0x0000F102u
#define RC_PID_CLT_C        0x0000F103u
#define RC_PID_IAT_C        0x0000F104u
#define RC_PID_OIL_C        0x0000F105u
#define RC_PID_LOAD_X10     0x0000F106u
#define RC_PID_TPS_X10      0x0000F107u
#define RC_PID_BAT_MV       0x0000F108u
#define RC_PID_F109_RETIRED 0x0000F109u   // F109 retired (was brake temp), never reuse

enum {
    IDX_SVC,
    IDX_CHAR_CAN_MAIN,
    IDX_CHAR_VAL_CAN_MAIN,
    IDX_CHAR_CFG_CAN_MAIN,
    IDX_CHAR_FILTER,
    IDX_CHAR_VAL_FILTER,
    IDX_NB,
};

// The pairing service uses its own attribute table (a separate create_attr_tab call), so it is
// an independently discoverable Primary Service in the peer's GATT database, with boundaries
// not mixed with the RaceChrono service.
enum {
    IDX_PAIR_SVC,
    IDX_PAIR_CHAR_MAC,
    IDX_PAIR_CHAR_VAL_MAC,
    IDX_PAIR_NB,
};

static const uint16_t s_attr_uuid_primary_service = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_attr_uuid_char_declare = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t s_attr_uuid_char_client_cfg = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t s_char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t s_char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t s_char_prop_read = ESP_GATT_CHAR_PROP_BIT_READ;

static uint16_t s_service_uuid = RC_SERVICE_UUID;
static uint16_t s_char_uuid_can_main = RC_CHAR_CAN_MAIN;
static uint16_t s_char_uuid_filter = RC_CHAR_FILTER;
static uint16_t s_cccd_init = 0x0000;
static uint8_t s_dummy_val[20] = {0};

static uint16_t s_pair_service_uuid = GAUGE_PAIR_SERVICE_UUID;
static uint16_t s_char_uuid_pair_mac = GAUGE_PAIR_CHAR_MAC;
static uint8_t s_pair_mac[6] = {0};   // this device's ESP-NOW/WiFi MAC; only meaningful in the MASTER role

// Advertising/GAP device name: the MASTER role advertises "SkyGauge-XXYY" (for slave-gauge pairing discovery), otherwise keeps "SkyGarageRC" (for the RaceChrono phone app).
static char s_adv_name[20] = RC_DEVICE_NAME;

static uint8_t s_adv_raw[] = {
    0x02, 0x01, 0x06,                    // Flags: LE General Discoverable + BR/EDR not supported
    0x05, 0x03, 0xF8, 0x1F, 0xF9, 0x1F   // Complete List of 16-bit Service UUIDs: 0x1FF8 (RC), 0x1FF9 (Pair)
};
// Pair-only advert (MASTER with RaceChrono off), built at runtime because the name varies:
// Flags + Pair UUID + Complete Local Name. The name is also in the main packet, not only in
// the scan response.
static uint8_t s_adv_raw_pair[31];
static uint8_t s_adv_raw_pair_len = 0;
static uint8_t s_scan_rsp_raw[31] = {0};

// Decide the advertising name and pairing MAC characteristic content based on the current device_role; called once before the startup flow / role change.
static void build_adv_identity(void)
{
    nvs_user_cfg_t cfg = nvs_cfg_read();
    if (cfg.device_role == ESPNOW_ROLE_MASTER) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        memcpy(s_pair_mac, mac, sizeof(s_pair_mac));
        snprintf(s_adv_name, sizeof(s_adv_name), "SkyGauge-%02X%02X", mac[4], mac[5]);
    } else {
        memset(s_pair_mac, 0, sizeof(s_pair_mac));
        strncpy(s_adv_name, RC_DEVICE_NAME, sizeof(s_adv_name) - 1);
        s_adv_name[sizeof(s_adv_name) - 1] = '\0';
    }
}

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x40,
    .adv_int_max = 0x80,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// Build scan response with complete local name.
// AD format: [len][type=0x09][name bytes...]
static uint32_t build_scan_rsp_with_name(void)
{
    size_t name_len = strlen(s_adv_name);
    if (name_len > 29) {
        name_len = 29;
    }
    memset(s_scan_rsp_raw, 0, sizeof(s_scan_rsp_raw));
    s_scan_rsp_raw[0] = (uint8_t)(1 + name_len);
    s_scan_rsp_raw[1] = 0x09;
    memcpy(&s_scan_rsp_raw[2], s_adv_name, name_len);
    return (uint32_t)(2 + name_len);
}

// Build the pair-only advert: Flags + Pair service UUID + Complete Local Name.
static void build_pair_adv_data(void)
{
    size_t name_len = strlen(s_adv_name);
    if (name_len > 22) {
        name_len = 22;   // keep total <= 31: 3 (flags) + 4 (uuid list) + 2 (name header) + name
    }
    uint8_t *p = s_adv_raw_pair;
    *p++ = 0x02; *p++ = 0x01; *p++ = 0x06;          // Flags
    *p++ = 0x03; *p++ = 0x03;                        // Complete List of 16-bit Service UUIDs
    *p++ = 0xF9; *p++ = 0x1F;                        // 0x1FF9 Pair
    *p++ = (uint8_t)(1 + name_len); *p++ = 0x09;     // Complete Local Name
    memcpy(p, s_adv_name, name_len);
    p += name_len;
    s_adv_raw_pair_len = (uint8_t)(p - s_adv_raw_pair);
}

static void request_adv_config(void)
{
    uint32_t scan_rsp_len = build_scan_rsp_with_name();

    s_adv_raw_done = false;
    s_scan_rsp_raw_done = false;
    s_adv_config_done = false;

    const uint8_t *adv_data;
    uint32_t adv_len;
    if (s_rc_enabled) {
        adv_data = s_adv_raw;
        adv_len = sizeof(s_adv_raw);
    } else {
        build_pair_adv_data();
        adv_data = s_adv_raw_pair;
        adv_len = s_adv_raw_pair_len;
    }

    esp_err_t err_adv = esp_ble_gap_config_adv_data_raw((uint8_t *)adv_data, adv_len);
    esp_err_t err_rsp = esp_ble_gap_config_scan_rsp_data_raw(s_scan_rsp_raw, scan_rsp_len);

    if (err_adv == ESP_OK && err_rsp == ESP_OK) {
        s_adv_cfg_pending = false;
        return;
    }

    s_adv_cfg_pending = true;
    ESP_LOGW(RC_TAG, "Config adv raw deferred: adv=%s scan_rsp=%s",
             esp_err_to_name(err_adv), esp_err_to_name(err_rsp));
}

static const esp_gatts_attr_db_t s_gatt_db[IDX_NB] = {
    [IDX_SVC] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_primary_service, ESP_GATT_PERM_READ,
      sizeof(uint16_t), sizeof(s_service_uuid), (uint8_t *)&s_service_uuid}},

    [IDX_CHAR_CAN_MAIN] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_char_declare, ESP_GATT_PERM_READ,
      sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&s_char_prop_read_notify}},

    [IDX_CHAR_VAL_CAN_MAIN] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_char_uuid_can_main, ESP_GATT_PERM_READ,
      sizeof(s_dummy_val), sizeof(uint8_t), s_dummy_val}},

    [IDX_CHAR_CFG_CAN_MAIN] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_char_client_cfg,
      ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      sizeof(uint16_t), sizeof(uint16_t), (uint8_t *)&s_cccd_init}},

    [IDX_CHAR_FILTER] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_char_declare, ESP_GATT_PERM_READ,
      sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&s_char_prop_write}},

    [IDX_CHAR_VAL_FILTER] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_char_uuid_filter, ESP_GATT_PERM_WRITE,
      sizeof(s_dummy_val), sizeof(uint8_t), s_dummy_val}},
};

// ---- Pairing service (separate attribute table; s_pair_mac is valid only in the MASTER role, read by the slave gauge during BLE pairing) ----
static const esp_gatts_attr_db_t s_gatt_db_pair[IDX_PAIR_NB] = {
    [IDX_PAIR_SVC] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_primary_service, ESP_GATT_PERM_READ,
      sizeof(uint16_t), sizeof(s_pair_service_uuid), (uint8_t *)&s_pair_service_uuid}},

    [IDX_PAIR_CHAR_MAC] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_char_declare, ESP_GATT_PERM_READ,
      sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&s_char_prop_read}},

    [IDX_PAIR_CHAR_VAL_MAC] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_char_uuid_pair_mac, ESP_GATT_PERM_READ,
      sizeof(s_pair_mac), sizeof(s_pair_mac), s_pair_mac}},
};

// Values use signal_store internal units; scale preserves the RaceChrono
// legacy wire format (load/TPS are tenths, battery is mV).
static const rc_chan_t s_channels[RC_CH_MAX] = {
    {RC_PID_RPM,          SIG_OBD_RPM,          1},
    {RC_PID_SPEED_KMH,    SIG_OBD_SPEED,        1},
    {RC_PID_CLT_C,        SIG_OBD_COOLANT_TEMP, 1},
    {RC_PID_IAT_C,        SIG_OBD_INTAKE_TEMP,  1},
    {RC_PID_OIL_C,        SIG_OBD_OIL_TEMP,     1},
    {RC_PID_LOAD_X10,     SIG_OBD_LOAD,        10},
    {RC_PID_TPS_X10,      SIG_OBD_THROTTLE,    10},
    {RC_PID_BAT_MV,       SIG_OBD_BATTERY_VOLTAGE, 1},
    {RC_PID_F109_RETIRED, SIG_SLOT_NONE,        1}, // F109 retired, never reuse
};

static inline void le32_store(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t be32_to_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static bool is_known_pid(uint32_t pid)
{
    for (int i = 0; i < RC_CH_MAX; i++) {
        if (s_channels[i].pid == pid) {
            return true;
        }
    }
    return false;
}

static bool map_index_to_pid(uint32_t idx, uint32_t *out_pid)
{
    if (out_pid == NULL) {
        return false;
    }

    // Compatibility: some clients send channel index instead of virtual CAN PID.
    // Accept both 0-based [0..RC_CH_MAX-1] and 1-based [1..RC_CH_MAX].
    if (idx < RC_CH_MAX) {
        *out_pid = s_channels[idx].pid;
        return true;
    }
    if (idx >= 1 && idx <= RC_CH_MAX) {
        *out_pid = s_channels[idx - 1].pid;
        return true;
    }
    return false;
}

static bool normalize_client_pid(uint32_t pid_raw, uint32_t *out_pid)
{
    if (out_pid == NULL) {
        return false;
    }

    uint32_t pid_le = pid_raw;
    uint32_t pid_be = ((pid_raw & 0x000000FFu) << 24) |
                      ((pid_raw & 0x0000FF00u) << 8) |
                      ((pid_raw & 0x00FF0000u) >> 8) |
                      ((pid_raw & 0xFF000000u) >> 24);

    if (is_known_pid(pid_le)) {
        *out_pid = pid_le;
        return true;
    }
    if (is_known_pid(pid_be)) {
        *out_pid = pid_be;
        return true;
    }
    if (map_index_to_pid(pid_le, out_pid)) {
        return true;
    }
    if (map_index_to_pid(pid_be, out_pid)) {
        return true;
    }
    return false;
}

static void send_can_packet(uint32_t pid, int32_t value)
{
    portENTER_CRITICAL(&s_filter_mux);
    bool enabled = s_connected && s_notify_enabled && s_handle_can_main != 0 && s_gatts_if != ESP_GATT_IF_NONE;
    uint16_t conn_id = s_conn_id;
    uint16_t handle = s_handle_can_main;
    esp_gatt_if_t gatts_if = s_gatts_if;
    portEXIT_CRITICAL(&s_filter_mux);
    if (!enabled || !ble_hub_is_ready()) return;

    uint8_t pkt[8];
    le32_store(pkt, pid);
    le32_store(pkt + 4, (uint32_t)value);
    esp_err_t err = esp_ble_gatts_send_indicate(gatts_if, conn_id, handle, sizeof(pkt), pkt, false);
    if (err != ESP_OK) {
        int64_t now_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_filter_mux);
        bool log_error = now_us - s_last_send_err_log_us > 1000000;
        if (log_error) s_last_send_err_log_us = now_us;
        portEXIT_CRITICAL(&s_filter_mux);
        if (log_error) {
            ESP_LOGW(RC_TAG, "send_indicate failed: %s", esp_err_to_name(err));
        }
    }
}

static void process_filter_write(const uint8_t *buf, uint16_t len)
{
    if (len < 1 || buf == NULL) {
        return;
    }

    uint8_t cmd = buf[0];
    if (cmd == 0) {
        portENTER_CRITICAL(&s_filter_mux);
        rc_filter_deny_all(&s_filter);
        portEXIT_CRITICAL(&s_filter_mux);
        wake_stream();
        ESP_LOGD(RC_TAG, "Filter: deny all");
        return;
    }

    if (cmd == 1) {
        portENTER_CRITICAL(&s_filter_mux);
        uint16_t interval_ms = len >= 3 ? (uint16_t)((buf[1] << 8) | buf[2]) : s_filter.all_interval_ms;
        rc_filter_allow_all(&s_filter, interval_ms);
        interval_ms = s_filter.all_interval_ms;
        portEXIT_CRITICAL(&s_filter_mux);
        wake_stream();
        ESP_LOGD(RC_TAG, "Filter: allow all, interval=%u ms", interval_ms);
        return;
    }

    if (cmd == 2 && len >= 7) {
        uint16_t interval_ms = (uint16_t)((buf[1] << 8) | buf[2]);
        uint32_t pid_le = ((uint32_t)buf[3]) |
                          ((uint32_t)buf[4] << 8) |
                          ((uint32_t)buf[5] << 16) |
                          ((uint32_t)buf[6] << 24);
        uint32_t pid_be = be32_to_u32(&buf[3]);
        uint32_t pid = 0;

        if (!normalize_client_pid(pid_le, &pid)) {
            ESP_LOGW(RC_TAG, "Filter: unknown PID raw_le=0x%08" PRIX32 " raw_be=0x%08" PRIX32 ", ignore", pid_le, pid_be);
            return;
        }
        if (interval_ms == 0) {
            interval_ms = 100;
        }

        for (unsigned i = 0; i < RC_CH_MAX; i++) {
            if (s_channels[i].pid != pid) continue;
            portENTER_CRITICAL(&s_filter_mux);
            rc_filter_subscribe(&s_filter, i, interval_ms);
            portEXIT_CRITICAL(&s_filter_mux);
            wake_stream();
            ESP_LOGD(RC_TAG, "Filter: allow PID=0x%08" PRIX32 " interval=%u", pid, interval_ms);
            break;
        }
    }
}

static void stream_task(void *arg)
{
    (void)arg;
    uint16_t live_channels = 0;
    for (unsigned i = 0; i < RC_CH_MAX; i++) {
        if (s_channels[i].slot != SIG_SLOT_NONE) live_channels |= (uint16_t)(1u << i);
    }
    int64_t sample_not_before_us = 0;
    while (1) {
        if (!ble_hub_is_ready()) {
            // BLE shut down for WiFi OTA (one-way): stop touching the stack.
            ESP_LOGI(RC_TAG, "BLE down, stream task exit");
            s_stream_task = NULL;
            vTaskDelete(NULL);
        }
        int64_t now_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_filter_mux);
        bool active = s_connected && s_notify_enabled && s_attr_ready;
        uint16_t due = active && now_us >= sample_not_before_us
                     ? rc_filter_due_mask(&s_filter, live_channels, now_us) : 0;
        portEXIT_CRITICAL(&s_filter_mux);
        if (due) {
            /* Plan active state and filter together before snapshotting:
             * idle intervals/deny-all perform no signal-store reads, and
             * F109 stays retired and is never sampled. */
            int slots[RC_CH_MAX];
            unsigned channels[RC_CH_MAX];
            int32_t values[RC_CH_MAX];
            size_t count = 0;
            for (unsigned i = 0; i < RC_CH_MAX; i++) {
                if (!(due & (1u << i)) || s_channels[i].slot == SIG_SLOT_NONE) continue;
                channels[count] = i;
                slots[count++] = s_channels[i].slot;
            }
            if (count) {
                /* Invalid samples do not claim a send interval, but repeated
                 * event wakeups must not turn them into a busy retry loop. */
                sample_not_before_us = now_us + 20000;
                PERF_BEGIN(perf_snapshot_start);
                uint64_t valid = signal_store_get_many(slots, count, values);
                PERF_END(PERF_RC_SNAPSHOT, perf_snapshot_start, (uint32_t)count);
                uint16_t valid_channels = 0;
                for (size_t i = 0; i < count; i++) {
                    if (valid & (UINT64_C(1) << i)) valid_channels |= (uint16_t)(1u << channels[i]);
                }
                portENTER_CRITICAL(&s_filter_mux);
                uint16_t send = s_connected && s_notify_enabled && s_attr_ready
                              ? rc_filter_claim_valid(&s_filter, valid_channels, now_us) : 0;
                portEXIT_CRITICAL(&s_filter_mux);
                for (size_t i = 0; i < count; i++) {
                    unsigned channel = channels[i];
                    if (!(send & (1u << channel))) continue;
                    send_can_packet(s_channels[channel].pid, values[i] * s_channels[channel].scale);
                }
            }
        }
        now_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_filter_mux);
        active = s_connected && s_notify_enabled && s_attr_ready;
        uint32_t wait_us = active ? rc_filter_next_wake_us(&s_filter, live_channels, now_us,
                                                        sample_not_before_us)
                                  : RC_FILTER_WAIT_FOREVER;
        portEXIT_CRITICAL(&s_filter_mux);
        /* Tokens coalesce and persist before the take: a GATT write or hub
         * shutdown between deadline planning and blocking cannot be lost. */
        xSemaphoreTake(s_stream_wake, stream_wait_ticks(wait_us));
    }
}

static void start_advertising_if_ready(void)
{
    if (!s_adv_config_done) {
        return;
    }

    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    if (err == ESP_OK) {
        s_adv_start_pending = false;
        return;
    }

    // If controller is busy, just defer without stopping external scans
    s_adv_start_pending = true;
    ESP_LOGW(RC_TAG, "Start adv deferred: %s", esp_err_to_name(err));
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(RC_TAG, "GATTS app register failed, status=%d", param->reg.status);
            break;
        }
        portENTER_CRITICAL(&s_filter_mux);
        s_gatts_if = gatts_if;
        portEXIT_CRITICAL(&s_filter_mux);
        build_adv_identity();
        {
            esp_err_t err = esp_ble_gap_set_device_name(s_adv_name);
            if (err != ESP_OK) {
                ESP_LOGW(RC_TAG, "Set device name failed: %s", esp_err_to_name(err));
            }
        }
        if (s_rc_enabled) {
            // Full mode: create both services (RC → Pair)
            request_adv_config();
            esp_ble_gatts_create_attr_tab(s_gatt_db, gatts_if, IDX_NB, 0);
            ESP_LOGD(RC_TAG, "GATTS registered (adv name=%s), waiting for adv+attr table", s_adv_name);
        } else {
            // Master minimal mode (racechrono_ble_diy_start() only registers a non-RC app on a
            // MASTER): advertise the Pair service so slaves can discover and pair
            request_adv_config();
            esp_ble_gatts_create_attr_tab(s_gatt_db_pair, gatts_if, IDX_PAIR_NB, 1);
            ESP_LOGI(RC_TAG, "GATTS registered (Master mode: Pair only, adv on, name=%s)", s_adv_name);
        }
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK &&
            param->add_attr_tab.num_handle == IDX_NB &&
            param->add_attr_tab.svc_uuid.uuid.uuid16 == RC_SERVICE_UUID) {
            uint16_t *h = param->add_attr_tab.handles;
            portENTER_CRITICAL(&s_filter_mux);
            s_handle_can_main = h[IDX_CHAR_VAL_CAN_MAIN];
            s_handle_can_cccd = h[IDX_CHAR_CFG_CAN_MAIN];
            s_handle_filter = h[IDX_CHAR_VAL_FILTER];
            portEXIT_CRITICAL(&s_filter_mux);
            esp_ble_gatts_start_service(h[IDX_SVC]);
            portENTER_CRITICAL(&s_filter_mux);
            s_attr_ready = true;
            portEXIT_CRITICAL(&s_filter_mux);
            wake_stream();
            ESP_LOGD(RC_TAG, "RC attr table ready, CAN main handle=0x%04X", s_handle_can_main);
            // After the RC service table is created, create the pairing service attribute table separately
            // (independent create_attr_tab calls, so both services are independently discoverable Primary Services in the peer's GATT database).
            esp_ble_gatts_create_attr_tab(s_gatt_db_pair, gatts_if, IDX_PAIR_NB, 1);
        } else if (param->add_attr_tab.status == ESP_GATT_OK &&
                   param->add_attr_tab.num_handle == IDX_PAIR_NB &&
                   param->add_attr_tab.svc_uuid.uuid.uuid16 == GAUGE_PAIR_SERVICE_UUID) {
            uint16_t *h = param->add_attr_tab.handles;
            esp_ble_gatts_start_service(h[IDX_PAIR_SVC]);
            ESP_LOGD(RC_TAG, "Pair attr table ready, service handle=0x%04X", h[IDX_PAIR_SVC]);
        } else {
            ESP_LOGE(RC_TAG, "Create attr table failed, status=%d uuid=0x%04X num_handle=%d",
                     param->add_attr_tab.status, param->add_attr_tab.svc_uuid.uuid.uuid16,
                     param->add_attr_tab.num_handle);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        // Bluedroid reports every LE link to every GATTS app, including the links this
        // device opens as a central (ELM327 adapter, gauge pairing): only a peer that
        // connected to us (we are the slave) is a RaceChrono / pairing client.
        if (param->connect.link_role != 1) {
            break;
        }
        portENTER_CRITICAL(&s_filter_mux);
        s_connected = true;
        s_conn_id = param->connect.conn_id;
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        // Start each connection from a safe default, so stale deny-all rules don't block data.
        rc_filter_reset(&s_filter);
        s_last_send_err_log_us = 0;
        portEXIT_CRITICAL(&s_filter_mux);
        wake_stream();
        ESP_LOGI(RC_TAG, "GATT client connected: %02x:%02x:%02x:%02x:%02x:%02x conn_id=0x%04X",
                 param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
                 param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5],
                 param->connect.conn_id);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        // Ignore drops of other links (our central links to the adapter / a master gauge).
        if (!s_connected || param->disconnect.conn_id != s_conn_id ||
            memcmp(param->disconnect.remote_bda, s_peer_bda, sizeof(esp_bd_addr_t)) != 0) {
            break;
        }
        portENTER_CRITICAL(&s_filter_mux);
        s_connected = false;
        s_notify_enabled = false;
        portEXIT_CRITICAL(&s_filter_mux);
        wake_stream();
        ESP_LOGI(RC_TAG, "GATT client disconnected: %02x:%02x:%02x:%02x:%02x:%02x",
                 param->disconnect.remote_bda[0], param->disconnect.remote_bda[1], param->disconnect.remote_bda[2],
                 param->disconnect.remote_bda[3], param->disconnect.remote_bda[4], param->disconnect.remote_bda[5]);
        start_advertising_if_ready();
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_handle_can_cccd && param->write.len >= 2) {
            uint16_t cccd = (uint16_t)param->write.value[0] | ((uint16_t)param->write.value[1] << 8);
            portENTER_CRITICAL(&s_filter_mux);
            s_notify_enabled = ((cccd & 0x0003u) != 0);
            portEXIT_CRITICAL(&s_filter_mux);
            wake_stream();
            ESP_LOGD(RC_TAG, "CAN main cccd=0x%04X stream %s", cccd, s_notify_enabled ? "EN" : "DIS");
        } else if (s_attr_ready && param->write.handle == s_handle_filter) {
            process_filter_write(param->write.value, param->write.len);
        }
        break;

    default:
        break;
    }
}

// GAP events fanned out by ble_hub (scan results go to the scan owner only).
static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(RC_TAG, "Adv raw data applied");
        s_adv_raw_done = true;
        if (s_adv_raw_done && s_scan_rsp_raw_done) {
            s_adv_config_done = true;
            s_adv_cfg_pending = false;
            start_advertising_if_ready();
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(RC_TAG, "Scan rsp raw data applied");
        s_scan_rsp_raw_done = true;
        if (s_adv_raw_done && s_scan_rsp_raw_done) {
            s_adv_config_done = true;
            s_adv_cfg_pending = false;
            start_advertising_if_ready();
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (s_adv_cfg_pending) {
            request_adv_config();
        }
        if (s_adv_start_pending) {
            start_advertising_if_ready();
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_adv_start_pending = false;
            ESP_LOGI(RC_TAG, "Advertising started (name=%s)", s_adv_name);
        } else {
            ESP_LOGW(RC_TAG, "Advertising start failed status=%d", param->adv_start_cmpl.status);
        }
        break;
    default:
        break;
    }
}

void racechrono_ble_diy_start(bool enable_racechrono)
{
    if (s_started) {
        return;
    }

    s_rc_enabled = enable_racechrono;

    // RaceChrono off and not MASTER: there is no service to publish (Pair is MASTER-only),
    // so no GATTS app is registered and the device does not advertise.
    if (!s_rc_enabled && nvs_cfg_read().device_role != ESPNOW_ROLE_MASTER) {
        ESP_LOGI(RC_TAG, "RaceChrono off and not MASTER: no GATTS services, skip start");
        return;
    }

    /* Initialize before registering callbacks; retained through OTA teardown. */
    if (s_rc_enabled && !s_stream_wake) s_stream_wake = xSemaphoreCreateBinaryStatic(&s_stream_wake_storage);

    // ble_hub brings the stack up if needed and registers GATTS app 0x52;
    // its REG_EVT does the common setup + service creation.
    const ble_hub_client_t client = {
        .gap_cb = gap_cb,
        .gattc_cb = NULL,
        .gatts_cb = gatts_cb,
        .shutdown_cb = wake_stream,
    };
    esp_err_t err = ble_hub_register(BLE_HUB_APP_RACECHRONO, &client);
    if (err != ESP_OK) {
        ESP_LOGE(RC_TAG, "ble_hub register failed: %s", esp_err_to_name(err));
        return;
    }

    if (s_rc_enabled && !s_stream_task) {
        s_stream_task = app_task_start(APP_TASK_RC_STREAM, stream_task, NULL);
    }

    s_started = true;
    if (s_rc_enabled) {
        ESP_LOGD(RC_TAG, "RaceChrono BLE DIY started");
        ESP_LOGD(RC_TAG, "PID map: RPM=0x%08X SPD=0x%08X CLT=0x%08X IAT=0x%08X OIL=0x%08X LOADx10=0x%08X TPSx10=0x%08X BATmV=0x%08X",
                 RC_PID_RPM, RC_PID_SPEED_KMH, RC_PID_CLT_C, RC_PID_IAT_C, RC_PID_OIL_C,
                 RC_PID_LOAD_X10, RC_PID_TPS_X10, RC_PID_BAT_MV);
    } else {
        ESP_LOGI(RC_TAG, "RaceChrono BLE DIY started (minimal: Pair only)");
    }
}

