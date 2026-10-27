#include "bsp_obd_dsp/obd_link.h"

#include "esp_log.h"

#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/elm327_wifi_client.h"
#include "bsp_obd_dsp/racechrono_ble_diy.h"

static const char *TAG = "obd_link";
static bool s_wifi;
static bool s_ble_started;   /* BLE stack initialized this boot */

static bool mac_nonzero(const uint8_t *m)
{
    return m && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

bool obd_link_start(const nvs_user_cfg_t *cfg, bool espnow_on)
{
    s_wifi = (cfg->conn_type == 1 && cfg->wifi_obd_ssid[0] != '\0');
    if (s_wifi) {
        /* No BLE stack at all: the single radio is dedicated to the STA link. */
        ESP_LOGI(TAG, "OBD transport: WiFi TCP '%s' (BLE stack released, ESP-NOW paused)",
                 cfg->wifi_obd_ssid);
        elm327_wifi_start_default(cfg->wifi_obd_ssid);
        s_ble_started = false;
        return true;
    }

    ESP_LOGI(TAG, "OBD transport: BLE");
    s_ble_started = true;
    if (mac_nonzero(cfg->ble_obd_mac)) {
        elm327_ble_start_default(cfg->ble_device_name, cfg->ble_obd_mac);
    } else if (cfg->ble_device_name[0] != '\0' || espnow_on) {
        /* Saved name without MAC, or MASTER needs the stack for pairing. */
        elm327_ble_ensure_stack_init();
    }
    return false;
}

bool obd_link_is_wifi(void)        { return s_wifi; }

bool obd_link_is_connected(void)
{
    return s_wifi ? elm327_wifi_is_connected() : elm327_ble_is_connected();
}

const char *obd_link_get_name(void)
{
    return s_wifi ? elm327_wifi_get_connected_name() : elm327_ble_get_connected_name();
}

void obd_link_set_demand_mask(uint16_t mask)
{
    if (s_wifi) elm327_wifi_set_obd_demand_mask(mask);
    else        elm327_ble_set_obd_demand_mask(mask);
}

void obd_link_pause_for_ota(void)
{
    if (s_ble_started) elm327_ble_pause_for_ota();
    elm327_wifi_pause_for_ota();
}

void obd_link_switch_to_ble(const uint8_t mac[6], const char *name)
{
    bool was_wifi = s_wifi;
    if (s_wifi) {
        elm327_wifi_disconnect();   /* close TCP + leave the adapter AP */
        s_wifi = false;
    }
    if (!s_ble_started) {
        elm327_ble_ensure_stack_init();
        racechrono_ble_diy_start(nvs_cfg_read().rc_enabled);
        s_ble_started = true;
    }
    elm327_ble_connect_by_addr(mac, name);
    ESP_LOGI(TAG, "runtime switch: BLE%s", was_wifi ? " (WiFi OBD stopped)" : "");
}

void obd_link_switch_to_wifi(const char *ssid)
{
    if (!ssid || !ssid[0]) return;
    s_wifi = true;
    elm327_wifi_start_default(ssid);   /* resumes paused tasks or rejoins a new AP */
    ESP_LOGI(TAG, "runtime switch: WiFi OBD '%s'", ssid);
}

void obd_link_stop_wifi(void)
{
    if (s_wifi) elm327_wifi_disconnect();
}
