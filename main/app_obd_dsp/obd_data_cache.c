#include "obd_data_cache.h"
#include "signal_store.h"
#include "vehicle/vp_profile.h"
#include "vehicle/vp_loader.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "bsp_obd_dsp/nvs_storage.h"


// Simple globals protected by a critical section
static int16_t  s_coolant_temp = -40;
static int16_t  s_oil_temp = -100;
static int16_t  s_intake_temp = -40;
static int16_t  s_load_pct = -1;
static int16_t  s_tps = -1;
static int32_t  s_bat_mv = -1;
static int16_t  s_oil_pressure_x10 = -1;
static int16_t  s_boost_x10 = -32768;
static int8_t   s_gear = 127;
static int16_t  s_afr_x100 = -1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#define SPEED_SMOOTH_TIME_MS 300   // speed ramp-up/down time constant (ms); the UI side already has anim_step, keep this small
#define FALL_TO_ZERO_MS      500   // fall-to-zero ramp-down time constant (ms)

// Smoothing state (advanced on the setter side, getters only read; unaffected by the number of callers)
static uint16_t s_rpm_smooth = 0;
static uint8_t  s_speed_smooth = 0;
static TickType_t s_speed_last_tick = 0;
static float s_speed_smooth_f = 0.f;

// RPM override layer: during multi-gauge linkage tests, the master gauge injects simulated RPM here.
// When enabled, obd_data_get_rpm() returns the override value (used for both local display and ESP-NOW broadcast).
static bool     s_rpm_override_en = false;
static uint16_t s_rpm_override_val = 0;

void obd_data_rpm_override_set(bool en, uint16_t val)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm_override_en = en;
    s_rpm_override_val = val;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_rpm(uint16_t rpm)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm_smooth = rpm;  // CAN 100Hz data is already clean, no smoothing needed
    portEXIT_CRITICAL(&s_mux);
    signal_store_set(SIG_OBD_RPM, (int32_t)rpm);
}

void obd_data_reset_temp_cache(void)
{
    portENTER_CRITICAL(&s_mux);
    s_coolant_temp = -40;
    s_oil_temp = -100;
    s_intake_temp = -40;
    portEXIT_CRITICAL(&s_mux);
    signal_store_invalidate(SIG_OBD_COOLANT_TEMP);
    signal_store_invalidate(SIG_OBD_OIL_TEMP);
    signal_store_invalidate(SIG_OBD_INTAKE_TEMP);
}

void obd_data_set_oil_temp_invalid(void)
{
    portENTER_CRITICAL(&s_mux);
    s_oil_temp = -100;
    portEXIT_CRITICAL(&s_mux);
    signal_store_invalidate(SIG_OBD_OIL_TEMP);
}

void obd_data_set_speed(uint8_t kmh)
{
    TickType_t now_tick = xTaskGetTickCount();
    uint32_t dt_ms = (now_tick - s_speed_last_tick) * portTICK_PERIOD_MS;
    if (dt_ms > 1000) dt_ms = 1000;
    s_speed_last_tick = now_tick;

    uint32_t tc = (kmh == 0) ? FALL_TO_ZERO_MS : SPEED_SMOOTH_TIME_MS;
    float alpha = (float)dt_ms / (float)tc;
    if (alpha > 1.0f) alpha = 1.0f;
    s_speed_smooth_f += alpha * ((float)kmh - s_speed_smooth_f);

    uint8_t smoothed = (uint8_t)(s_speed_smooth_f + 0.5f);
    portENTER_CRITICAL(&s_mux);
    s_speed_smooth = smoothed;
    portEXIT_CRITICAL(&s_mux);
    signal_store_set(SIG_OBD_SPEED, (int32_t)smoothed);
}

void obd_data_set_coolant_temp(int16_t temp)
{
    portENTER_CRITICAL(&s_mux);
    s_coolant_temp = temp;
    portEXIT_CRITICAL(&s_mux);
    /* -40 is the initial sentinel (never-received); treat as invalid */
    if (temp == -40) signal_store_invalidate(SIG_OBD_COOLANT_TEMP);
    else             signal_store_set(SIG_OBD_COOLANT_TEMP, (int32_t)temp);
}

void obd_data_set_oil_temp(int16_t temp)
{
    if (temp < -20 || temp > 150) return;
    portENTER_CRITICAL(&s_mux);
    s_oil_temp = temp;
    portEXIT_CRITICAL(&s_mux);
    signal_store_set(SIG_OBD_OIL_TEMP, (int32_t)temp);
}

void obd_data_set_intake_temp(int16_t temp)
{
    portENTER_CRITICAL(&s_mux);
    s_intake_temp = temp;
    portEXIT_CRITICAL(&s_mux);
    if (temp == -40) signal_store_invalidate(SIG_OBD_INTAKE_TEMP);
    else             signal_store_set(SIG_OBD_INTAKE_TEMP, (int32_t)temp);
}

uint16_t obd_data_get_rpm(void)
{
    uint16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_rpm_override_en ? s_rpm_override_val : s_rpm_smooth;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

int16_t obd_data_get_coolant_temp(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_coolant_temp;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

int16_t obd_data_get_intake_temp(void)
{
    int16_t val;
    portENTER_CRITICAL(&s_mux);
    val = s_intake_temp;
    portEXIT_CRITICAL(&s_mux);
    return val;
}

void obd_data_set_load_pct(int16_t pct)
{
    portENTER_CRITICAL(&s_mux);
    s_load_pct = pct;
    portEXIT_CRITICAL(&s_mux);
    if (pct == -1) signal_store_invalidate(SIG_OBD_LOAD);
    else           signal_store_set(SIG_OBD_LOAD, (int32_t)pct);
}

void obd_data_set_tps(int16_t pct)
{
    portENTER_CRITICAL(&s_mux);
    s_tps = pct;
    portEXIT_CRITICAL(&s_mux);
    if (pct == -1) signal_store_invalidate(SIG_OBD_TPS);
    else           signal_store_set(SIG_OBD_TPS, (int32_t)pct);
}

void obd_data_set_bat_mv(int32_t mv)
{
    portENTER_CRITICAL(&s_mux);
    s_bat_mv = mv;
    portEXIT_CRITICAL(&s_mux);
    if (mv == -1) signal_store_invalidate(SIG_OBD_BAT_MV);
    else          signal_store_set(SIG_OBD_BAT_MV, mv);
}

void obd_data_set_oil_pressure_x10(int16_t pressure_x10)
{
    if (pressure_x10 < 0 || pressure_x10 > 200) {
        signal_store_invalidate(SIG_OBD_OIL_PRESSURE);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_oil_pressure_x10 = pressure_x10;
    portEXIT_CRITICAL(&s_mux);
    signal_store_set(SIG_OBD_OIL_PRESSURE, (int32_t)pressure_x10);
}

void obd_data_set_boost_x10(int16_t boost_x10)
{
    if (boost_x10 < -15 || boost_x10 > 300) {
        signal_store_invalidate(SIG_OBD_BOOST);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_boost_x10 = boost_x10;
    portEXIT_CRITICAL(&s_mux);
    signal_store_set(SIG_OBD_BOOST, (int32_t)boost_x10);
}

void obd_data_set_gear(int8_t gear)
{
    portENTER_CRITICAL(&s_mux);
    s_gear = gear;
    portEXIT_CRITICAL(&s_mux);
    if (gear == 127) signal_store_invalidate(SIG_OBD_GEAR);
    else             signal_store_set(SIG_OBD_GEAR, (int32_t)gear);
}

void obd_data_set_afr_x100(int16_t afr_x100)
{
    if (afr_x100 < 800 || afr_x100 > 2200) {
        signal_store_invalidate(SIG_OBD_AFR);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_afr_x100 = afr_x100;
    portEXIT_CRITICAL(&s_mux);
    signal_store_set(SIG_OBD_AFR, (int32_t)afr_x100);
}

void obd_data_get_snapshot(obd_data_snapshot_t *out)
{
    if (!out) return;

    portENTER_CRITICAL(&s_mux);
    out->rpm = s_rpm_override_en ? s_rpm_override_val : s_rpm_smooth;
    out->speed = s_speed_smooth;
    out->coolant_temp = s_coolant_temp;
    out->oil_temp = s_oil_temp;
    out->intake_temp = s_intake_temp;
    out->load_pct = s_load_pct;
    out->tps = s_tps;
    out->bat_mv = s_bat_mv;
    out->oil_pressure_x10 = s_oil_pressure_x10;
    out->boost_x10 = s_boost_x10;
    out->gear = s_gear;
    out->afr_x100 = s_afr_x100;
    portEXIT_CRITICAL(&s_mux);
}

/**
 * @brief Compute and determine the gear from RPM and vehicle speed
 * @param rpm engine speed (RPM)
 * @param speed vehicle speed (km/h)
 * @return the computed gear
 */
enGear calculate_gear(float rpm, float speed) {
    static enGear s_last_gear = GEAR_NEUTRAL;
    // 1. Check input data validity
    if (rpm <= 0 || speed <= 0) {
        s_last_gear = GEAR_NEUTRAL;
        return GEAR_NEUTRAL;
    }

    // 2. Compute the total gear ratio using the active vehicle profile
    const vp_profile_t *profile = vp_loader_get_active();
    float calc_const = vehicle_profile_calc_constant(profile);
    float total_ratio = rpm / (speed * calc_const);

    // 3. Compare against each gear's ratio range
    uint8_t range_count = 0;
    const gear_ratio_range_t *ranges = vehicle_profile_get_gear_ranges(&range_count);
    for (int i = 0; i < range_count; i++) {
        if (total_ratio >= ranges[i].min_ratio &&
            total_ratio <= ranges[i].max_ratio) {
            s_last_gear = ranges[i].gear;
            return ranges[i].gear;
        }
    }
    
    // 4. Outside all ranges: check if it could be neutral (high RPM, near-zero speed)
    if (rpm > 800 && speed < 5) { // Above idle and nearly stationary
        s_last_gear = GEAR_NEUTRAL;
        return GEAR_NEUTRAL;
    }
    
    // 5. Unrecognized ratio: return the last gear
    return s_last_gear;
}


/**
 * @brief Mileage statistics timer callback
 * @param pvParameter argument
 * @return none
 * @note
 * @note Mileage statistics task
 */
