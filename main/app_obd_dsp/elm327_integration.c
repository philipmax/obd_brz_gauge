/**
 * elm327_integration.c
 *
 * Integration glue: wire vp_scheduler into elm327_ble_client poll task
 * to replace the hardcoded 12-slot switch.
 */

#include "elm327_integration.h"
#include "vehicle/vp_scheduler.h"
#include "vehicle/vp_loader.h"
#include "signal_store.h"
#include "obd_data_cache.h"
#include "lifecycle_state.h"
#include "../bsp_obd_dsp/elm327_ble_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "elm327_integ";
static vp_scheduler_t s_scheduler;
static bool s_initialized = false;

// Callback adapters: vp_scheduler writes to signal_store, we read and forward to obd_data_cache
static void sync_signal_to_cache(signal_id_t sig_id)
{
    if (!signal_store_is_valid(sig_id)) return;
    int32_t value = signal_store_value(sig_id);

    switch (sig_id) {
        case SIG_OBD_RPM:
            obd_data_set_rpm((uint16_t)value);
            break;
        case SIG_OBD_SPEED:
            obd_data_set_speed((uint8_t)value);
            break;
        case SIG_OBD_COOLANT_TEMP:
            obd_data_set_coolant_temp((int16_t)value);
            break;
        case SIG_OBD_OIL_TEMP:
            obd_data_set_oil_temp((int16_t)value);
            break;
        case SIG_OBD_GEAR:
            obd_data_set_gear((int8_t)value);
            break;
        case SIG_OBD_THROTTLE:
            obd_data_set_throttle_position((uint8_t)value);
            break;
        case SIG_OBD_LOAD:
            obd_data_set_load_pct((uint8_t)value);
            break;
        case SIG_OBD_INTAKE_TEMP:
            obd_data_set_intake_temp((int16_t)value);
            break;
        case SIG_OBD_OIL_PRESSURE:
            obd_data_set_oil_pressure((uint16_t)value);
            break;
        case SIG_OBD_MANIFOLD_PRESSURE:
            obd_data_set_manifold_pressure((uint16_t)value);
            break;
        case SIG_OBD_AFR:
            obd_data_set_afr((uint16_t)value);
            break;
        default:
            break;
    }
}

void elm327_integration_tick(void)
{
    if (!s_initialized) return;

    // Only run when ELM327 is connected
    lifecycle_state_t state = lifecycle_get_state();
    if (state != LIFECYCLE_ELM_CONNECTED) {
        return;
    }

    // Tick the scheduler (it will decide what to request based on rate limits)
    if (!vp_scheduler_tick(&s_scheduler)) {
        return;  // Not ready to send
    }

    // Get pending command
    const char *cmd = vp_scheduler_get_next_cmd(&s_scheduler);
    if (cmd && elm327_ble_is_connected()) {
        // Send to ELM327
        if (elm327_ble_send_ascii_blocking(cmd)) {
            ESP_LOGD(TAG, "-> %s", cmd);
        }
    }

    // Sync all OBD signals to obd_data_cache
    for (signal_id_t sig = SIG_OBD_RPM; sig < SIG_OBD_COUNT; sig++) {
        sync_signal_to_cache(sig);
    }
}

void elm327_integration_on_response(const char *response)
{
    if (!s_initialized) return;
    // Feed response to scheduler for decoding
    vp_scheduler_feed_response(&s_scheduler, response, strlen(response));
}

void elm327_integration_init(void)
{
    // Initialize scheduler with active profile
    const vp_profile_t *profile = vp_loader_get_active();
    if (!profile) {
        ESP_LOGE(TAG, "No active profile");
        return;
    }

    esp_err_t err = vp_scheduler_init(&s_scheduler, profile);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scheduler init failed: %s", esp_err_to_name(err));
        return;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "ELM327 integration initialized with profile: %s", profile->name);
}
