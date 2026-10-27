#pragma once
/*
 * obd_link - picks the OBD transport for this boot (BLE or ELM327-over-WiFi)
 * and exposes the small status/demand API the UI needs, so screens do not
 * have to know which transport is active.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bsp_obd_dsp/nvs_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the configured OBD transport. Returns true when WiFi OBD owns the
 * radio, in which case ESP-NOW must not be started. */
bool obd_link_start(const nvs_user_cfg_t *cfg, bool espnow_on);

bool        obd_link_is_wifi(void);
bool        obd_link_is_connected(void);
const char *obd_link_get_name(void);
void        obd_link_set_demand_mask(uint16_t mask);
void        obd_link_pause_for_ota(void);

/* Runtime transport switching used by the connection pages:
 * switch_to_ble stops WiFi OBD and (re)starts the BLE client + RaceChrono;
 * switch_to_wifi resumes/rejoins the already-active WiFi transport;
 * stop_wifi drops the STA/TCP link without changing the mode flag. */
void        obd_link_switch_to_ble(const uint8_t mac[6], const char *name);
void        obd_link_switch_to_wifi(const char *ssid);
void        obd_link_stop_wifi(void);

#ifdef __cplusplus
}
#endif
