/* Original production delivered-value formula, independent of fast paths. */
#include "delivery_reference.h"
#include "app_obd_dsp/signal_registry.h"
#include <math.h>

double delivery_reference(int slot, int32_t value)
{
    if (slot >= SIG_STD_COUNT) return (double)value;
    const sig_def_t *d = &sig_registry[slot];
    double x;
    switch (d->dlv_op) {
    case SIG_DLV_MUL: x = (double)value * d->dlv_by; break;
    case SIG_DLV_DIV: x = (double)value / d->dlv_by; break;
    default:          return (double)value;
    }
    switch (d->dlv_round) {
    case SIG_RND_HALF_UP: return floor(x + 0.5);
    case SIG_RND_FLOOR:   return floor(x);
    default:              return x;
    }
}
