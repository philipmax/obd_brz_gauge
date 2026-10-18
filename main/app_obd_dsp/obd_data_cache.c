#include "obd_data_cache.h"
#include "signal_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SPEED_SMOOTH_TIME_MS 300   // speed ramp-up/down time constant (ms); the UI side already has anim_step, keep this small
#define FALL_TO_ZERO_MS      500   // fall-to-zero ramp-down time constant (ms)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Speed smoothing state (advanced on the setter side; single writer)
static TickType_t s_speed_last_tick = 0;
static float s_speed_smooth_f = 0.f;

// RPM override layer: during multi-gauge linkage tests, the master gauge injects simulated RPM here.
static bool     s_rpm_override_en = false;
static uint16_t s_rpm_override_val = 0;

static inline int32_t div10_half_up(int32_t v)
{
    return v >= 0 ? (v + 5) / 10 : -((-v + 5) / 10);
}

static inline void set_or_invalidate(int slot, bool invalid, int32_t internal)
{
    if (invalid) signal_store_invalidate(slot);
    else         signal_store_set(slot, internal);
}

void obd_data_rpm_override_set(bool en, uint16_t val)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm_override_en = en;
    s_rpm_override_val = val;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_rpm(uint16_t rpm)
{
    signal_store_set(SIG_OBD_RPM, (int32_t)rpm);  // CAN 100Hz data is already clean, no smoothing needed
}

static uint16_t rpm_with_override(uint16_t rpm)
{
    portENTER_CRITICAL(&s_mux);
    const bool override_en = s_rpm_override_en;
    const uint16_t override_val = s_rpm_override_val;
    portEXIT_CRITICAL(&s_mux);
    return override_en ? override_val : rpm;
}

uint16_t obd_data_get_rpm(void)
{
    int32_t rpm = 0;
    if (!signal_store_get(SIG_OBD_RPM, &rpm) || rpm < 0) rpm = 0;
    if (rpm > UINT16_MAX) rpm = UINT16_MAX;
    return rpm_with_override((uint16_t)rpm);
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

    signal_store_set(SIG_OBD_SPEED, (int32_t)(s_speed_smooth_f + 0.5f));
}

void obd_data_set_coolant_temp(int16_t temp)
{
    /* -40 is the initial sentinel (never-received); treat as invalid */
    set_or_invalidate(SIG_OBD_COOLANT_TEMP, temp == -40, temp);
}

void obd_data_set_oil_temp(int16_t temp)
{
    if (temp < -20 || temp > 150) return;
    signal_store_set(SIG_OBD_OIL_TEMP, temp);
}

void obd_data_set_intake_temp(int16_t temp)
{
    set_or_invalidate(SIG_OBD_INTAKE_TEMP, temp == -40, temp);
}

void obd_data_set_load_pct(int16_t pct)
{
    set_or_invalidate(SIG_OBD_LOAD, pct < 0, pct);
}

void obd_data_set_tps(int16_t pct)
{
    set_or_invalidate(SIG_OBD_THROTTLE, pct < 0, pct);
}

void obd_data_set_oil_pressure_x10(int16_t pressure_x10)
{
    /* legacy 0.1 bar -> internal 0.01 bar */
    set_or_invalidate(SIG_OBD_OIL_PRESSURE, pressure_x10 < 0, (int32_t)pressure_x10 * 10);
}

void obd_data_set_boost_x10(int16_t boost_x10)
{
    set_or_invalidate(SIG_OBD_BOOST, boost_x10 == -32768, (int32_t)boost_x10 * 10);
}

void obd_data_set_afr_x100(int16_t afr_x100)
{
    set_or_invalidate(SIG_OBD_AFR, afr_x100 < 0, afr_x100);
}

void obd_data_get_motion_snapshot(obd_motion_snapshot_t *out)
{
    if (!out) return;
    static const int slots[] = {SIG_OBD_RPM, SIG_OBD_SPEED, SIG_OBD_GEAR};
    int32_t v[sizeof(slots) / sizeof(slots[0])];
    uint64_t valid = signal_store_get_many(slots, sizeof(slots) / sizeof(slots[0]), v);

    int32_t rpm = (valid & (UINT64_C(1) << 0)) ? v[0] : 0;
    if (rpm > UINT16_MAX) rpm = UINT16_MAX;
    out->rpm = rpm_with_override((uint16_t)rpm);
    out->speed = (uint8_t)((valid & (UINT64_C(1) << 1)) ?
                           (v[1] > UINT8_MAX ? UINT8_MAX : v[1]) : 0);
    out->gear = (int8_t)((valid & (UINT64_C(1) << 2)) ? v[2] : OBD_GEAR_UNKNOWN);
}

void obd_data_get_snapshot(obd_data_snapshot_t *out)
{
    if (!out) return;

    enum {
        SNAP_RPM, SNAP_SPEED, SNAP_CLT, SNAP_OIL, SNAP_IAT, SNAP_LOAD,
        SNAP_TPS, SNAP_BAT, SNAP_OIL_PRESSURE, SNAP_BOOST, SNAP_GEAR, SNAP_AFR,
        SNAP_COUNT
    };
    static const int slots[SNAP_COUNT] = {
        SIG_OBD_RPM, SIG_OBD_SPEED, SIG_OBD_COOLANT_TEMP, SIG_OBD_OIL_TEMP,
        SIG_OBD_INTAKE_TEMP, SIG_OBD_LOAD, SIG_OBD_THROTTLE,
        SIG_OBD_BATTERY_VOLTAGE, SIG_OBD_OIL_PRESSURE, SIG_OBD_BOOST,
        SIG_OBD_GEAR, SIG_OBD_AFR
    };
    int32_t v[SNAP_COUNT];
    uint64_t valid = signal_store_get_many(slots, SNAP_COUNT, v);

    int32_t rpm = (valid & (1ULL << SNAP_RPM)) ? v[SNAP_RPM] : 0;
    if (rpm > UINT16_MAX) rpm = UINT16_MAX;
    out->rpm = rpm_with_override((uint16_t)rpm);
    out->speed = (uint8_t)(v[SNAP_SPEED] > 255 ? 255 : v[SNAP_SPEED]);
    out->coolant_temp = (valid & (1ULL << SNAP_CLT)) ? (int16_t)v[SNAP_CLT] : -40;
    out->oil_temp = (valid & (1ULL << SNAP_OIL)) ? (int16_t)v[SNAP_OIL] : -100;
    out->intake_temp = (valid & (1ULL << SNAP_IAT)) ? (int16_t)v[SNAP_IAT] : -40;
    out->load_pct = (valid & (1ULL << SNAP_LOAD)) ? (int16_t)v[SNAP_LOAD] : -1;
    out->tps = (valid & (1ULL << SNAP_TPS)) ? (int16_t)v[SNAP_TPS] : -1;
    out->bat_mv = (valid & (1ULL << SNAP_BAT)) ? v[SNAP_BAT] : -1;
    out->oil_pressure_x10 = (valid & (1ULL << SNAP_OIL_PRESSURE)) ?
                            (int16_t)div10_half_up(v[SNAP_OIL_PRESSURE]) : -1;
    out->boost_x10 = (valid & (1ULL << SNAP_BOOST)) ?
                     (int16_t)div10_half_up(v[SNAP_BOOST]) : -32768;
    out->gear = (int8_t)((valid & (1ULL << SNAP_GEAR)) ? v[SNAP_GEAR] : OBD_GEAR_UNKNOWN);
    out->afr_x100 = (int16_t)((valid & (1ULL << SNAP_AFR)) ? v[SNAP_AFR] : -1);
}

void obd_data_get_snapshot_mask(obd_data_snapshot_t *out, uint16_t requested)
{
    if (!out) return;
    _Static_assert(SIG_OBD_FIRST == 0 && SIG_OBD_COUNT <= 16,
                   "snapshot mask requires OBD slots to fit in uint16_t");
    const uint16_t obd_mask = (uint16_t)((1u << SIG_OBD_COUNT) - 1u);
    requested &= obd_mask;
    if (requested == obd_mask) {
        // Keep the existing full-snapshot path free of sparse planning overhead.
        obd_data_get_snapshot(out);
        return;
    }
    *out = (obd_data_snapshot_t){
        .coolant_temp = -40, .oil_temp = -100, .intake_temp = -40,
        .load_pct = -1, .tps = -1, .bat_mv = -1, .oil_pressure_x10 = -1,
        .boost_x10 = -32768, .gear = OBD_GEAR_UNKNOWN, .afr_x100 = -1
    };
    if (!requested) return;

    int slots[SIG_OBD_COUNT];
    int32_t values[SIG_OBD_COUNT];
    size_t count = 0;
    unsigned pending = requested;
    while (pending) {
        slots[count++] = __builtin_ctz(pending);
        pending &= pending - 1u;
    }
    const uint64_t valid = signal_store_get_many(slots, count, values);
    for (size_t i = 0; i < count; i++) {
        if (!(valid & (UINT64_C(1) << i))) continue;
        const int32_t v = values[i];
        switch (slots[i]) {
        case SIG_OBD_RPM: out->rpm = (uint16_t)(v > UINT16_MAX ? UINT16_MAX : v); break;
        case SIG_OBD_SPEED: out->speed = (uint8_t)(v > UINT8_MAX ? UINT8_MAX : v); break;
        case SIG_OBD_COOLANT_TEMP: out->coolant_temp = (int16_t)v; break;
        case SIG_OBD_OIL_TEMP: out->oil_temp = (int16_t)v; break;
        case SIG_OBD_INTAKE_TEMP: out->intake_temp = (int16_t)v; break;
        case SIG_OBD_LOAD: out->load_pct = (int16_t)v; break;
        case SIG_OBD_THROTTLE: out->tps = (int16_t)v; break;
        case SIG_OBD_BATTERY_VOLTAGE: out->bat_mv = v; break;
        case SIG_OBD_OIL_PRESSURE: out->oil_pressure_x10 = (int16_t)div10_half_up(v); break;
        case SIG_OBD_BOOST: out->boost_x10 = (int16_t)div10_half_up(v); break;
        case SIG_OBD_GEAR: out->gear = (int8_t)v; break;
        case SIG_OBD_AFR: out->afr_x100 = (int16_t)v; break;
        }
    }
    if (requested & (1u << SIG_OBD_RPM)) out->rpm = rpm_with_override(out->rpm);
}
