/*
 * elm327_wifi_client.c - ELM327 WiFi (TCP) transport + its "obd_poll" task.
 * Contract: elm327_wifi_client.h.
 *
 * Threads:
 *   - wifi_link: STA join (fixed AP password), TCP connect/reconnect, recv()
 *     into the shared RX stream buffer; the only writer of the socket;
 *   - obd_poll: the only caller of vp_engine (single command in flight, never
 *     interleaved), exactly like the BLE transport;
 *   - scan worker: short-lived, runs one esp_wifi_scan and reports APs.
 * ESP-NOW is not started while this transport is selected (app_main skips it),
 * because the STA link owns the radio channel.
 */

#include "app_obd_dsp/perf_stats.h"
#include "bsp_obd_dsp/elm327_wifi_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/signal_store.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "vehicle/vp_engine.h"
#include "vehicle/vp_manager.h"

extern bool ui_showroom_is_active(void);   /* export_path/ui_ext.c */

static const char *TAG = "elm327_wifi";

/* Runtime link diagnostics are off by default: periodic UART logging steals
 * serial/CPU time from the OBD poll task. Set ELM327_WIFI_DIAG_LOG=1 to enable. */
#ifndef ELM327_WIFI_DIAG_LOG
#define ELM327_WIFI_DIAG_LOG 0
#endif

#define TASK_STACK             5120
#define TASK_PRIO              4
#define RX_BUF_BYTES           1024
#define RX_CHUNK               128
#define RX_DRAIN_MAX           16
#define TX_LINE_MAX            64
#define IDLE_WAIT_MS           300
#define SHOWROOM_WAIT_MS       500
#define OTA_WAIT_MS            200
#define SETTLE_US              (300LL * 1000)
#define STATS_EVERY_US         (60LL * 1000 * 1000)
#define TCP_CONNECT_TIMEOUT_MS 5000
#define TCP_CHUNK_TIMEOUT_MS   1000
#define WIFI_JOIN_TIMEOUT_MS   15000
#define LINK_RETRY_MS          2000

#define EV_GOT_IP       BIT0
#define EV_DISCONNECTED BIT1
#define EV_SCAN_DONE    BIT2

static const char *TAG_ENGINE = "obd";

/* ---- WiFi / socket state ---- */
static bool                s_wifi_ready;
static EventGroupHandle_t  s_ev;
static volatile int        s_sock = -1;
static volatile bool       s_tcp_up;
static volatile bool       s_paused;
static volatile bool       s_started;
static volatile bool       s_scanning;
static volatile bool       s_scan_stop;
static char                s_ssid[33];
static char                s_host[16];
static uint16_t            s_port;
static wifi_scan_found_cb_t s_scan_cb;
static int                 s_scan_secs;

/* ---- poll task / engine ---- */
static volatile TaskHandle_t s_task;
static StreamBufferHandle_t  s_rx;
static vp_engine_t          *s_engine;
static bool                  s_engine_up;
static uint16_t              s_obd_demand_mask;
static volatile bool         s_quiet;
static volatile int64_t      s_up_us;
static volatile uint32_t     s_rx_dropped;

/* ---- runtime diagnostics (never logs OBD payloads) ---- */
static volatile uint32_t     s_rx_bytes;
static volatile uint32_t     s_tx_cmds;
static volatile uint32_t     s_tcp_fails;
static volatile uint32_t     s_reconnects;
static volatile int64_t      s_last_rx_us;
#define STATUS_EVERY_US        (5LL * 1000 * 1000)
#define STALL_WARN_US          (3LL * 1000 * 1000)

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void wake_task(void)
{
    TaskHandle_t t = s_task;
    if (t) xTaskNotifyGive(t);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
            /* reason 8 = we left on purpose (reconfig / disconnect) */
            if (d && d->reason != WIFI_REASON_ASSOC_LEAVE) {
                ESP_LOGW(TAG, "STA disconnected (reason %d)", d->reason);
            }
            xEventGroupSetBits(s_ev, EV_DISCONNECTED);
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            xEventGroupSetBits(s_ev, EV_SCAN_DONE);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_ev, EV_GOT_IP);
}

static bool ensure_event_group(void)
{
    if (s_ev) return true;
    s_ev = xEventGroupCreate();
    if (!s_ev) return false;
    esp_err_t e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);
    if (e != ESP_OK) return false;
    e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
    return e == ESP_OK;
}

/* Idempotent: ESP-NOW / OTA may already have initialized the driver. */
static bool wifi_ensure_started(void)
{
    if (!s_wifi_ready) {
        esp_err_t e = esp_netif_init();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(e));
            return false;
        }
        e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(e));
            return false;
        }
        if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") &&
            !esp_netif_create_default_wifi_sta()) {
            ESP_LOGE(TAG, "STA netif create failed");
            return false;
        }
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        e = esp_wifi_init(&cfg);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(e));
            return false;
        }
        s_wifi_ready = true;
    }
    if (!ensure_event_group()) return false;

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    return true;
}

static bool wifi_join(void)
{
    if (!wifi_ensure_started()) return false;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        strcmp((const char *)ap.ssid, s_ssid) == 0) {
        return true;
    }

    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISCONNECTED);
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_OBD_AP_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* adapter APs may be open or WPA2 */
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;

    esp_wifi_disconnect();
    if (esp_wifi_connect() != ESP_OK) return false;
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_DISCONNECTED, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(WIFI_JOIN_TIMEOUT_MS));
    return (b & EV_GOT_IP) != 0;
}

/* ------------------------------------------------------------------ */
/* TCP                                                                 */
/* ------------------------------------------------------------------ */

static void close_sock(void)
{
    int s = s_sock;
    if (s >= 0) {
        s_sock = -1;
        close(s);
    }
    s_tcp_up = false;
    wake_task();
}

static int tcp_open(void)
{
    esp_ip4_addr_t ip;
    if (esp_netif_str_to_ip4(s_host, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "bad host '%s'", s_host);
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) return -1;

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(s_port);
    dest.sin_addr.s_addr = ip.addr;

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&dest, sizeof(dest));
    if (r < 0 && errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv = { .tv_sec = TCP_CONNECT_TIMEOUT_MS / 1000,
                              .tv_usec = (TCP_CONNECT_TIMEOUT_MS % 1000) * 1000 };
        r = select(s + 1, NULL, &wf, NULL, &tv);
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (r > 0) getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (r <= 0 || soerr) r = -1;
    }
    if (r < 0) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval rtv = { .tv_sec = TCP_CHUNK_TIMEOUT_MS / 1000, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));
    return s;
}

#if ELM327_WIFI_DIAG_LOG
/* Periodic runtime summary: link/RSSI/RX rate/reconnect counters/engine link
 * and heap. Never prints OBD command or response payloads. */
static void log_link_status(uint32_t *last_rx_bytes, int64_t *last_log_us)
{
    int64_t now = esp_timer_get_time();
    int64_t dt_us = now - *last_log_us;
    if (dt_us <= 0) dt_us = 1;
    *last_log_us = now;
    uint32_t rx = s_rx_bytes - *last_rx_bytes;
    *last_rx_bytes = s_rx_bytes;

    wifi_ap_record_t ap;
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    uint8_t eng_link = 0;
    if (s_engine) {
        vp_engine_stats_t st = {0};
        vp_engine_get_stats(s_engine, &st);
        eng_link = st.link;
    }
    int64_t idle_ms = s_last_rx_us ? (now - s_last_rx_us) / 1000 : -1;

    ESP_LOGI(TAG, "link %s ssid='%s' rssi=%d, rx %lu B/s, tx %lu, drops %lu, tcp_fail %lu, "
                  "reconn %lu, engine_link %u, idle %lld ms, int free %u, psram free %u",
             s_tcp_up ? "up" : "down", s_ssid, rssi,
             (unsigned long)((uint64_t)rx * 1000000u / (uint64_t)dt_us),
             (unsigned long)s_tx_cmds, (unsigned long)s_rx_dropped,
             (unsigned long)s_tcp_fails, (unsigned long)s_reconnects, eng_link,
             (long long)idle_ms,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (s_tcp_up && idle_ms > STALL_WARN_US / 1000) {
        ESP_LOGW(TAG, "no TCP data for %lld ms (rssi %d)", (long long)idle_ms, rssi);
    }
}
#endif   /* ELM327_WIFI_DIAG_LOG */

static void link_task(void *arg)
{
    (void)arg;
    uint8_t buf[RX_CHUNK];
#if ELM327_WIFI_DIAG_LOG
    uint32_t last_rx_bytes = 0;
    int64_t last_log_us = esp_timer_get_time();
#endif

    for (;;) {
#if ELM327_WIFI_DIAG_LOG
        int64_t now = esp_timer_get_time();
        if (now - last_log_us >= STATUS_EVERY_US) {
            log_link_status(&last_rx_bytes, &last_log_us);
        }
#endif
        if (s_paused || !s_started) {
            close_sock();
            vTaskDelay(pdMS_TO_TICKS(OTA_WAIT_MS));
            continue;
        }
        if (s_sock < 0) {
            if (!wifi_join()) {
                vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS));
                continue;
            }
            int s = tcp_open();
            if (s < 0) {
                s_tcp_fails++;
                ESP_LOGW(TAG, "TCP %s:%u connect failed", s_host, (unsigned)s_port);
                vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS));
                continue;
            }
            s_sock = s;
            s_tcp_up = true;
            s_up_us = esp_timer_get_time();
            s_last_rx_us = s_up_us;
            ESP_LOGI(TAG, "connected to %s:%u via '%s'", s_host, (unsigned)s_port, s_ssid);
            wake_task();
        }

        int n = recv(s_sock, buf, sizeof(buf), 0);
        if (n > 0) {
            s_rx_bytes += (uint32_t)n;
            s_last_rx_us = esp_timer_get_time();
            size_t sent = xStreamBufferSend(s_rx, buf, (size_t)n, pdMS_TO_TICKS(100));
            if (sent < (size_t)n) s_rx_dropped += (uint32_t)((size_t)n - sent);
            wake_task();
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        } else {
            s_reconnects++;
            ESP_LOGW(TAG, "TCP closed (recv=%d errno=%d), retrying", n, errno);
            close_sock();
            vTaskDelay(pdMS_TO_TICKS(LINK_RETRY_MS));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Wi-Fi scan (UI selects the adapter hotspot)                         */
/* ------------------------------------------------------------------ */

static void scan_task(void *arg)
{
    (void)arg;
    uint16_t num = 0;
    wifi_ap_record_t *recs = NULL;

    if (wifi_ensure_started()) {
        xEventGroupClearBits(s_ev, EV_SCAN_DONE);
        if (esp_wifi_scan_start(NULL, false) == ESP_OK) {
            uint32_t wait_ms = (uint32_t)((s_scan_secs > 0 ? s_scan_secs : 10) * 1000);
            EventBits_t b = xEventGroupWaitBits(s_ev, EV_SCAN_DONE, pdTRUE, pdFALSE,
                                                pdMS_TO_TICKS(wait_ms));
            if (!(b & EV_SCAN_DONE)) esp_wifi_scan_stop();
            if (esp_wifi_scan_get_ap_num(&num) == ESP_OK && num > 0) {
                if (num > WIFI_SCAN_MAX_DEVICES) num = WIFI_SCAN_MAX_DEVICES;
                recs = calloc(num, sizeof(*recs));
                if (recs && esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
                    for (uint16_t i = 0; i < num && !s_scan_stop; i++) {
                        if (!recs[i].ssid[0]) continue;
                        wifi_scan_result_t r = {0};
                        strlcpy(r.ssid, (const char *)recs[i].ssid, sizeof(r.ssid));
                        r.rssi = recs[i].rssi;
                        if (s_scan_cb) s_scan_cb(&r, (int)num);
                    }
                }
            }
        }
    }
    free(recs);
    s_scanning = false;
    vTaskDelete(NULL);
}

void elm327_wifi_scan_start(int duration_s, wifi_scan_found_cb_t cb)
{
    if (s_scanning) return;
    s_scan_cb = cb;
    s_scan_secs = duration_s;
    s_scan_stop = false;
    s_scanning = true;
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL) != pdPASS) {
        s_scanning = false;
        s_scan_cb = NULL;
        ESP_LOGW(TAG, "scan task create failed");
    }
}

void elm327_wifi_scan_stop(void)
{
    s_scan_stop = true;
    if (s_scanning) esp_wifi_scan_stop();
    s_scan_cb = NULL;
}

/* ------------------------------------------------------------------ */
/* vp_engine sink                                                      */
/* ------------------------------------------------------------------ */

static bool sink_send(void *ctx, const char *cmd)
{
    (void)ctx;
    int s = s_sock;
    if (s < 0 || !s_tcp_up || s_paused) return false;

    char buf[TX_LINE_MAX];
    size_t n = strnlen(cmd, sizeof(buf) - 1);
    memcpy(buf, cmd, n);
    buf[n++] = '\r';
    size_t off = 0;
    while (off < n) {
        int w = send(s, buf + off, n - off, 0);
        if (w <= 0) {
            ESP_LOGW(TAG, "send failed (errno=%d)", errno);
            return false;
        }
        off += (size_t)w;
    }
    s_tx_cmds++;
    return true;
}

static uint32_t sink_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void sink_set_signal(void *ctx, int slot, int32_t internal)
{
    (void)ctx;
    if (!s_quiet) signal_store_set(slot, internal);
}

static void sink_invalidate(void *ctx, int slot)
{
    (void)ctx;
    if (!s_quiet) signal_store_invalidate(slot);
}

static void sink_set_speed(void *ctx, uint8_t kmh)
{
    (void)ctx;
    if (!s_quiet) obd_data_set_speed(kmh);
}

static void sink_on_link(void *ctx, uint8_t state)
{
    (void)ctx;
    if (!s_quiet) signal_store_set(SIG_SYS_OBD_LINK, state);
}

static int sink_veh_register(void *ctx, const char *key, int32_t min, int32_t max,
                             uint32_t stale_ms)
{
    (void)ctx;
    return signal_store_veh_register(key, min, max, stale_ms);
}

static void sink_veh_clear(void *ctx)
{
    (void)ctx;
    signal_store_veh_clear();
}

static void sink_on_response(void *ctx, int slot)
{
    (void)ctx;
    vp_manager_on_response(slot);
}

static void sink_on_voltage(void *ctx, float volts)
{
    (void)ctx;
    vp_manager_on_voltage(volts);
}

static void sink_on_elm_link(void *ctx, bool up)
{
    (void)ctx;
    vp_manager_on_elm_link(up);
}

static void sink_on_proto(void *ctx, uint8_t proto)
{
    (void)ctx;
    nvs_user_cfg_t base;
    nvs_cfg_snapshot(&base);
    if (base.protocol == proto) return;
    nvs_user_cfg_t cfg = base;
    cfg.protocol = proto;
    if (nvs_cfg_update(&base, &cfg) == ESP_OK) ESP_LOGI(TAG, "protocol %u detected and saved", proto);
}

static void sink_reconnect(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "no data after repeated re-init, reconnecting");
    close_sock();
}

static void sink_log(void *ctx, const char *msg)
{
    (void)ctx;
    ESP_LOGI(TAG_ENGINE, "%s", msg);
}

static const vp_engine_sink_t k_sink = {
    .ctx               = NULL,
    .send              = sink_send,
    .now_ms            = sink_now_ms,
    .set_signal        = sink_set_signal,
    .invalidate        = sink_invalidate,
    .set_speed_kmh     = sink_set_speed,
    .on_link           = sink_on_link,
    .veh_register      = sink_veh_register,
    .veh_clear         = sink_veh_clear,
    .on_response       = sink_on_response,
    .on_voltage        = sink_on_voltage,
    .on_elm_link       = sink_on_elm_link,
    .on_proto_detected = sink_on_proto,
    .reconnect         = sink_reconnect,
    .log               = sink_log,
};

static void *psram_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void psram_free(void *ctx, void *ptr)
{
    (void)ctx;
    heap_caps_free(ptr);
}

static const vp_allocator_t k_alloc = { psram_alloc, psram_free, NULL };

/* ------------------------------------------------------------------ */
/* obd_poll task                                                       */
/* ------------------------------------------------------------------ */

static bool engine_create(void)
{
    if (s_engine) return true;
    nvs_user_cfg_t nv = nvs_cfg_read();
    vp_engine_cfg_t cfg = { 0 };
    cfg.proto = (nv.protocol <= 12) ? nv.protocol : 0;
    s_engine = vp_engine_create(vp_manager_active(), &k_sink, &cfg, &k_alloc);
    if (!s_engine) {
        ESP_LOGE(TAG, "vp_engine_create failed (profile %s)", vp_manager_active_id());
        return false;
    }
    vp_engine_set_obd_demand_mask(s_engine, s_obd_demand_mask);
    ESP_LOGI(TAG, "engine ready: profile %s, protocol %u%s", vp_manager_active_id(), cfg.proto,
             cfg.proto ? "" : " (auto)");
    return true;
}

static void engine_down(void)
{
    if (s_engine && s_engine_up) vp_engine_link_down(s_engine);
    s_engine_up = false;
}

static void rx_discard(void)
{
    if (s_rx) xStreamBufferReset(s_rx);
}

static void rx_drain(void)
{
    uint8_t buf[RX_CHUNK];
    for (int i = 0; i < RX_DRAIN_MAX; i++) {
        size_t n = xStreamBufferReceive(s_rx, buf, sizeof(buf), 0);
        if (n == 0) break;
        vp_engine_on_rx(s_engine, buf, n);
    }
}

static void log_stats(void)
{
    vp_engine_stats_t st = { 0 };
    if (s_engine) vp_engine_get_stats(s_engine, &st);
    ESP_LOGD(TAG, "phase %u link %u proto %u, tx %lu req %lu ok %lu fail %lu timeout %lu heal %lu, "
                  "rx dropped %lu",
             st.phase, st.link, st.proto,
             (unsigned long)st.tx_cmds, (unsigned long)st.tx_req, (unsigned long)st.rx_ok,
             (unsigned long)st.rx_fail, (unsigned long)st.timeouts, (unsigned long)st.heals,
             (unsigned long)s_rx_dropped);
}

static void obd_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    engine_create();
    int64_t last_stats = esp_timer_get_time();
    uint32_t wait_ms = IDLE_WAIT_MS;

    for (;;) {
        esp_task_wdt_reset();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time();

        if (s_paused) {
            engine_down();
            rx_discard();
            wait_ms = OTA_WAIT_MS;
            continue;
        }
        if (ui_showroom_is_active()) {
            s_quiet = true;
            engine_down();
            s_quiet = false;
            rx_discard();
            wait_ms = SHOWROOM_WAIT_MS;
            continue;
        }
        if (!s_tcp_up) {
            engine_down();
            rx_discard();
            wait_ms = IDLE_WAIT_MS;
            continue;
        }
        if (!s_engine_up) {
            int64_t settle = s_up_us + SETTLE_US - now;
            if (settle > 0) {
                wait_ms = (uint32_t)(settle / 1000) + 1;
                continue;
            }
            if (!engine_create()) {
                wait_ms = 1000;
                continue;
            }
            rx_discard();
            vp_engine_link_up(s_engine);
            s_engine_up = true;
        }
        PERF_BEGIN(perf_elm_start);
        rx_drain();
        vp_engine_set_obd_demand_mask(s_engine, s_obd_demand_mask);
        wait_ms = vp_engine_run(s_engine);
        PERF_END(PERF_ELM_RUN, perf_elm_start, 1);

        if (now - last_stats > STATS_EVERY_US) {
            last_stats = now;
            log_stats();
        }
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void elm327_wifi_start_default(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        ESP_LOGW(TAG, "no WiFi OBD SSID configured");
        return;
    }
    bool ssid_changed = strcmp(s_ssid, ssid) != 0;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    nvs_user_cfg_t nv = nvs_cfg_read();
    strlcpy(s_host, nv.wifi_obd_host[0] ? nv.wifi_obd_host : WIFI_OBD_DEFAULT_HOST,
            sizeof(s_host));
    s_port = nv.wifi_obd_port ? nv.wifi_obd_port : WIFI_OBD_DEFAULT_PORT;

    s_paused = false;   /* resume a previously stopped/OTA-paused link */
    if (s_started) {
        if (ssid_changed) close_sock();   /* link_task rejoins the new AP */
        wake_task();
        return;
    }
    if (!s_rx) {
        s_rx = xStreamBufferCreateWithCaps(RX_BUF_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_rx) {
        ESP_LOGE(TAG, "RX buffer create failed");
        return;
    }

    TaskHandle_t t = NULL;
    if (xTaskCreate(obd_task, "obd_poll", TASK_STACK, NULL, TASK_PRIO, &t) != pdPASS) {
        ESP_LOGE(TAG, "obd_poll create failed");
        return;
    }
    s_task = t;
    if (xTaskCreate(link_task, "wifi_link", 4096, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "wifi_link create failed");
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "WiFi OBD started: '%s' -> %s:%u", s_ssid, s_host, (unsigned)s_port);
}

bool elm327_wifi_is_connected(void)
{
    return s_tcp_up;
}

const char *elm327_wifi_get_connected_name(void)
{
    return s_ssid;
}

void elm327_wifi_set_obd_demand_mask(uint16_t mask)
{
    s_obd_demand_mask = mask;
    if (s_engine) vp_engine_set_obd_demand_mask(s_engine, mask);
}

void elm327_wifi_disconnect(void)
{
    close_sock();
    s_paused = true;   /* stop auto-reconnect until the next start */
    esp_wifi_disconnect();
}

void elm327_wifi_pause_for_ota(void)
{
    s_paused = true;
    close_sock();
    esp_wifi_disconnect();
}
