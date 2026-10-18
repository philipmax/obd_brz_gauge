/* Real private sample pipeline; no hooks enter the firmware. */
#include "write_rounding_probe.h"
#ifndef WR_ENGINE_SOURCE
#define WR_ENGINE_SOURCE "../../../main/vehicle/vp_engine.c"
#endif
#include WR_ENGINE_SOURCE

bool wr_identity(float value, int32_t *out) { return round_identity(value, out); }
bool wr_reference(float value, int32_t *out)
{
    double rounded = floor((double)value + 0.5);
    if (!(rounded >= -2147483648.0 && rounded <= 2147483647.0)) return false;
    *out = (int32_t)rounded;
    return true;
}

/* Pre-fast-path rules: double rounding for every unit factor. */
static void reference_write_value(vp_engine_t *e, int i, float v, uint32_t now, bool from_can)
{
    const vp_signal_t *s = &e->sig[i];
    eng_sig_t *g = &e->sigs[i];
    double x;
    int32_t internal;

    if (g->slot == SIG_OBD_SPEED && e->p->speed_scale > 0.0f) v *= e->p->speed_scale;
    /* P6 post: clamp -> zero_if_rpm_lt -> zero_le */
    if (s->flags & VP_SIG_F_CLAMP) {
        if (v < s->clamp_lo) v = s->clamp_lo;
        if (v > s->clamp_hi) v = s->clamp_hi;
    }
    if (s->flags & VP_SIG_F_ZERO_RPM) {
        int32_t rpm = e->rpm_valid ? e->rpm : 0;
        if (rpm < (int32_t)s->zero_if_rpm_lt) v = 0.0f;
    }
    if ((s->flags & VP_SIG_F_ZERO_LE) && v <= s->zero_le) v = 0.0f;
    /* P7 signal valid */
    if ((s->flags & VP_SIG_F_VALID) && (v < s->valid_lo || v > s->valid_hi)) {
        sig_invalidate(e, i, now);
        return;
    }
    /* P8 internal units. Most standard signals use an identity factor; keep
     * that hot path in single precision and avoid the Xtensa double helpers.
     * The general path preserves the profile's double-precision rounding. */
    x = floor((double)v * g->factor + 0.5);
    if (!(x >= -2147483648.0 && x <= 2147483647.0)) {
        sig_invalidate(e, i, now);
        return;
    }
    internal = (int32_t)x;
    /* P9 registry range (veh.* ranges are enforced by signal_store) */
    if (g->slot < SIG_OBD_COUNT &&
        (internal < sig_registry[g->slot].min || internal > sig_registry[g->slot].max)) {
        sig_invalidate(e, i, now);
        return;
    }
    /* P10 filter + write */
    if (s->filter == VP_FILTER_TEMP_HYST) {
        if (!filter_temp_hyst(g, internal, from_can, &internal)) return;
    } else if (s->filter == VP_FILTER_EMA) {
        internal = filter_ema(g, internal);
    }
    store(e, i, internal, now);
}

struct wr_trace { unsigned writes, invalids, links; int slot; int32_t value; };
static void wr_set(void *ctx, int slot, int32_t value)
{
    struct wr_trace *t = ctx;
    t->writes++; t->slot = slot; t->value = value;
}
static void wr_invalid(void *ctx, int slot)
{
    struct wr_trace *t = ctx;
    t->invalids++; t->slot = slot;
}
static void wr_speed(void *ctx, uint8_t value) { wr_set(ctx, SIG_OBD_SPEED, value); }
static void wr_link(void *ctx, uint8_t value)
{
    struct wr_trace *t = ctx;
    t->links++; (void)value;
}
bool wr_compare(float value, unsigned config, unsigned index)
{
    static const double factors[] = {1.0, 100.0, 1000.0, 0.01, 1.609344};
    vp_engine_t e, before, expected;
    vp_profile_t p;
    vp_signal_t signals[VP_MAX_SIGNALS];
    struct wr_trace traces[2];
    unsigned mode = config % 8, f = (config / 8) % 5;
    eng_sig_t *g;
    if (index >= VP_MAX_SIGNALS) abort();
    memset(&e, 0, sizeof(e)); memset(&p, 0, sizeof(p));
    memset(signals, 0, sizeof(signals)); memset(traces, 0, sizeof(traces));
    p.n_sig = (uint16_t)(index + 1); p.speed_scale = config & 128 ? 1.1f : 1.0f;
    e.p = &p; e.sig = signals; e.sig_gear = -1;
    e.rpm_valid = (config & 64) != 0; e.rpm = 850;
    e.speed_valid = true; e.speed = 30; e.gear_valid = true; e.gear = 3;
    e.sink.ctx = &traces[0]; e.sink.set_signal = wr_set;
    e.sink.set_speed_kmh = wr_speed; e.sink.invalidate = wr_invalid; e.sink.on_link = wr_link;
    g = &e.sigs[index];
    g->slot = (int16_t)((config / 40) % 13);
    if (g->slot == 12) g->slot = SIG_STD_COUNT;
    g->factor = factors[f]; g->can_stale = true;
    if (f == 0) e.factor_identity_mask = UINT64_C(1) << index;
    signals[index].clamp_lo = -20.0f; signals[index].clamp_hi = 150.0f;
    signals[index].valid_lo = -10.0f; signals[index].valid_hi = 120.0f;
    signals[index].zero_le = 5.0f; signals[index].zero_if_rpm_lt = 900;
    switch (mode) {
    case 1: signals[index].flags = VP_SIG_F_CLAMP; break;
    case 2: signals[index].flags = VP_SIG_F_VALID; break;
    case 3: signals[index].flags = VP_SIG_F_ZERO_LE; break;
    case 4: signals[index].flags = VP_SIG_F_ZERO_RPM; break;
    case 5:
        signals[index].flags = VP_SIG_F_CLAMP; signals[index].filter = VP_FILTER_EMA;
        g->filt_init = true; g->filt = 40.0f; break;
    case 6:
        signals[index].flags = VP_SIG_F_CLAMP; signals[index].filter = VP_FILTER_TEMP_HYST;
        g->filt_init = true; g->filt = 40.0f; g->hyst_pending = ENG_HYST_NONE; break;
    case 7:
        signals[index].flags = VP_SIG_F_CLAMP | VP_SIG_F_VALID | VP_SIG_F_ZERO_LE | VP_SIG_F_ZERO_RPM;
        break;
    default: break;
    }
    before = e;
    reference_write_value(&e, (int)index, value, UINT32_MAX - config, (config & 256) != 0);
    expected = e; e = before;
    e.sink.ctx = &traces[1];
    write_value(&e, (int)index, value, UINT32_MAX - config, (config & 256) != 0);
    /* Both calls use the same immutable inputs and callback functions. */
    e.sink.ctx = expected.sink.ctx;
    return !memcmp(&e, &expected, sizeof(e)) && !memcmp(&traces[0], &traces[1], sizeof(traces[0]));
}
