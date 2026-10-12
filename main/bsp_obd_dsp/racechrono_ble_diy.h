#pragma once

#include <stdbool.h>
#include "esp_gap_ble_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start RaceChrono BLE DIY GATTS service (UUID 0x1FF8) and the gauge pairing
// service (0x1FF9). Requires BLE controller + bluedroid to be enabled by caller.
// When enable_racechrono is false, RC (0x1FF8) is skipped: a MASTER publishes and
// advertises only Pair (0x1FF9); any other role registers no GATTS app and stays
// invisible (no advertising).
void racechrono_ble_diy_start(bool enable_racechrono);

// Forward GAP callbacks from the app's single BLE GAP callback.
void racechrono_ble_diy_handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

#ifdef __cplusplus
}
#endif
