#include "delivery_reference.h"
#include "app_obd_dsp/signal_delivery.h"
double delivery_candidate(int slot, int32_t value)
{
    return signal_delivery_value(slot, value);
}
