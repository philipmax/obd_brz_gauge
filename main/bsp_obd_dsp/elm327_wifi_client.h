#pragma once
/*
 * elm327_wifi_client - ELM327 over WiFi (TCP) transport.
 *
 * The gauge joins the adapter's own AP (password fixed, see
 * WIFI_OBD_AP_PASSWORD), then speaks the same ELM327 command stream over a TCP
 * socket (default 192.168.0.10:35000). The "obd_poll" task drives vp_engine
 * exactly like the BLE transport, so profiles, protocol auto-detect and the
 * signal store behave identically.
 *
 * ESP-NOW is not started while this transport is selected (see app_main); the
 * STA link owns the radio channel.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_OBD_DEFAULT_HOST   "192.168.0.10"
#define WIFI_OBD_DEFAULT_PORT   35000
#define WIFI_OBD_AP_PASSWORD    "12345678"

#define WIFI_SCAN_MAX_DEVICES   20

typedef struct {
    char   ssid[33];
    int8_t rssi;
} wifi_scan_result_t;

/* Called from the scan worker for every AP (make UI updates LVGL-thread-safe). */
typedef void (*wifi_scan_found_cb_t)(const wifi_scan_result_t *ap, int total_count);

/* Scan nearby APs for duration_s seconds (UI selects the adapter's hotspot). */
void elm327_wifi_scan_start(int duration_s, wifi_scan_found_cb_t cb);
void elm327_wifi_scan_stop(void);

/* Start the transport for ssid: STA join + TCP connect + obd_poll task.
 * Host/port come from NVS (wifi_obd_host / wifi_obd_port, defaults above). */
void elm327_wifi_start_default(const char *ssid);

bool elm327_wifi_is_connected(void);
const char *elm327_wifi_get_connected_name(void);

/* Submit the UI signal-demand mask to the engine (same contract as BLE). */
void elm327_wifi_set_obd_demand_mask(uint16_t mask);

/* User asked to drop the link: close TCP and stop auto-reconnect. */
void elm327_wifi_disconnect(void);

/* WiFi OTA: drop STA/TCP and stop reconnecting; the WiFi driver stays up for
 * the SoftAP. OTA ends with a reboot, so this is one-way. */
void elm327_wifi_pause_for_ota(void);

#ifdef __cplusplus
}
#endif
