#pragma once
/* Pure delivered-unit conversion. Kept outside the signal-store lock. */
#include "signal_registry.h"
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Both callers compare positive finite literals. A representation comparison
 * keeps the metadata guard out of the target soft-double arithmetic path. */
static inline bool signal_delivery_scale_matches(const double *value, double scale)
{
    return memcmp(value, &scale, sizeof(scale)) == 0;
}

static inline double signal_delivery_value(int slot, int32_t value)
{
    if (slot >= SIG_STD_COUNT) return (double)value;
    const sig_def_t *d = &sig_registry[slot];
    double x;
    switch (d->dlv_op) {
    case SIG_DLV_MUL:
        /* Registry oil-pressure conversion: 0.145038 = 72519 / 500000.
         * Over its 0..2000 kPa range, half-up is exactly this integer ratio.
         * Check metadata too so a regenerated registry keeps other formulas. */
        if (d->dlv_round == SIG_RND_HALF_UP && signal_delivery_scale_matches(&d->dlv_by, 0.145038) &&
            (uint32_t)value <= 2000u) {
            return (double)(((uint32_t)value * 72519u + 250000u) / 500000u);
        }
        x = (double)value * d->dlv_by;
        break;
    case SIG_DLV_DIV:
        /* Battery voltage: floor(mV / 100). Handle negative inputs as well
         * without floating-point division or changing floor's semantics. */
        if (d->dlv_round == SIG_RND_FLOOR && signal_delivery_scale_matches(&d->dlv_by, 100.0)) {
            return (double)(value / 100 - (value % 100 < 0));
        }
        x = (double)value / d->dlv_by;
        break;
    default:
        return (double)value;
    }
    switch (d->dlv_round) {
    case SIG_RND_HALF_UP: return floor(x + 0.5);
    case SIG_RND_FLOOR:   return floor(x);
    default:              return x;
    }
}
