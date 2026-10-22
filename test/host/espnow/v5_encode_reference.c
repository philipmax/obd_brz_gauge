/* Frozen pre-optimization v5 encoder; test/benchmark only. */
#include "espnow_v6.h"
#include <string.h>
/* v5 DATA field offsets (packed little-endian struct, 47 bytes). */
#define V5_OFS_FLAGS      3
#define V5_OFS_SEQ        4
#define V5_OFS_RPM        8
#define V5_OFS_SPEED      10
#define V5_OFS_SWEEP      11
#define V5_OFS_INTRO      12
#define V5_OFS_COOLANT    13
#define V5_OFS_INTAKE     15
#define V5_OFS_OIL_TEMP   17
#define V5_OFS_OIL_P      19
#define V5_OFS_BOOST      21
/*      ESPNOW_V5_RETIRED_OFS 23: retired brake temperature, int16 = -1000 */
#define V5_OFS_LOAD       25
#define V5_OFS_TPS        27
#define V5_OFS_BAT        29
#define V5_OFS_AFR        33
#define V5_OFS_NAME       35


/* Legacy (v5 / obd_data_cache) sentinels. */
#define V5_TEMP_INVALID   (-40)    /* coolant / intake */
#define V5_OIL_T_INVALID  (-100)
#define V5_OIL_T_MIN      (-20)    /* obd_data_set_oil_temp(): outside -> hold */
#define V5_OIL_T_MAX      150
#define V5_BOOST_INVALID  (-32768)

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static void put_i16(uint8_t *p, int32_t v)
{
    if (v < INT16_MIN) v = INT16_MIN;
    if (v > INT16_MAX) v = INT16_MAX;
    put_u16(p, (uint16_t)(int16_t)v);
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Same rounding as obd_data_get_snapshot() (internal 0.01 -> legacy 0.1). */
static int32_t div10_half_up(int32_t v)
{
    return v >= 0 ? (v + 5) / 10 : -((-v + 5) / 10);
}


static void put_name(uint8_t *dst, const char *name)
{
    memset(dst, 0, ESPNOW_NAME_LEN);
    size_t n = 0;
    while (n < ESPNOW_NAME_LEN && name[n] != '\0') n++;
    memcpy(dst, name, n);
}

bool v5_snapshot_get_reference(const espnow_snapshot_t *s, uint16_t num_id, int32_t *out)
{
    if (!s || !out) return false;
    if (num_id == ESPNOW_ID_GEAR) {
        if (s->gear == ESPNOW_GEAR_INVALID) return false;
        *out = s->gear;
        return true;
    }
    for (int i = 0; i < s->n_rec; i++) {
        if (s->rec[i].num_id == num_id) {
            *out = s->rec[i].value;
            return true;
        }
    }
    return false;
}

size_t v5_encode_reference(const espnow_snapshot_t *s, uint8_t *out, size_t cap)
{
    if (!s || !out || cap < ESPNOW_V5_DATA_LEN) return 0;
    memset(out, 0, ESPNOW_V5_DATA_LEN);

    int32_t v = 0;
    put_u16(out, ESPNOW_MAGIC);
    out[2] = ESPNOW_V5_VERSION;
    out[V5_OFS_FLAGS] = s->flags;
    put_u32(out + V5_OFS_SEQ, s->seq);

    v = v5_snapshot_get_reference(s, ESPNOW_ID_RPM, &v) ? clamp32(v, 0, UINT16_MAX) : 0;
    put_u16(out + V5_OFS_RPM, (uint16_t)v);
    v = v5_snapshot_get_reference(s, ESPNOW_ID_SPEED, &v) ? clamp32(v, 0, UINT8_MAX) : 0;
    out[V5_OFS_SPEED] = (uint8_t)v;
    out[V5_OFS_SWEEP] = s->sweep_step;
    out[V5_OFS_INTRO] = s->intro_step;

    put_i16(out + V5_OFS_COOLANT,
            v5_snapshot_get_reference(s, ESPNOW_ID_COOLANT_TEMP, &v) ? v : V5_TEMP_INVALID);
    put_i16(out + V5_OFS_INTAKE,
            v5_snapshot_get_reference(s, ESPNOW_ID_INTAKE_TEMP, &v) ? v : V5_TEMP_INVALID);
    put_i16(out + V5_OFS_OIL_TEMP,
            v5_snapshot_get_reference(s, ESPNOW_ID_OIL_TEMP, &v) ? v : V5_OIL_T_INVALID);
    put_i16(out + V5_OFS_OIL_P,
            v5_snapshot_get_reference(s, ESPNOW_ID_OIL_PRESSURE, &v)
                ? clamp32(div10_half_up(v), 0, INT16_MAX) : -1);
    put_i16(out + V5_OFS_BOOST,
            v5_snapshot_get_reference(s, ESPNOW_ID_BOOST, &v)
                ? clamp32(div10_half_up(v), V5_BOOST_INVALID + 1, INT16_MAX) : V5_BOOST_INVALID);
    put_i16(out + ESPNOW_V5_RETIRED_OFS, ESPNOW_V5_RETIRED_VALUE);   /* retired: never reuse */
    put_i16(out + V5_OFS_LOAD,
            v5_snapshot_get_reference(s, ESPNOW_ID_LOAD, &v) ? clamp32(v, 0, INT16_MAX) : -1);
    put_i16(out + V5_OFS_TPS,
            v5_snapshot_get_reference(s, ESPNOW_ID_THROTTLE, &v) ? clamp32(v, 0, INT16_MAX) : -1);
    v = v5_snapshot_get_reference(s, ESPNOW_ID_BATTERY_VOLTAGE, &v) ? clamp32(v, 0, INT32_MAX) : -1;
    put_u32(out + V5_OFS_BAT, (uint32_t)v);
    put_i16(out + V5_OFS_AFR,
            v5_snapshot_get_reference(s, ESPNOW_ID_AFR, &v) ? clamp32(v, 0, INT16_MAX) : -1);
    put_name(out + V5_OFS_NAME, s->name);
    return ESPNOW_V5_DATA_LEN;
}
