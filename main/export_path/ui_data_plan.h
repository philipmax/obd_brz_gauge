#pragma once
// Pure display-source planning; no LVGL objects or signal-store reads.
#include <stddef.h>
#include <stdint.h>
#include "ui_disp_item.h"
#include "app_obd_dsp/signal_registry.h"

// Match the display-item mapping (including the permanently retired slot).
static inline uint16_t ui_data_mask_for_item(uint8_t item)
{
    static const uint16_t masks[DISP_ITEM_COUNT] = {
        [DISP_ITEM_CLT] = 1u << SIG_OBD_COOLANT_TEMP,
        [DISP_ITEM_IAT] = 1u << SIG_OBD_INTAKE_TEMP,
        [DISP_ITEM_OIL] = 1u << SIG_OBD_OIL_TEMP,
        [DISP_ITEM_LOAD] = 1u << SIG_OBD_LOAD,
        [DISP_ITEM_TPS] = 1u << SIG_OBD_THROTTLE,
        [DISP_ITEM_RPM] = 1u << SIG_OBD_RPM,
        [DISP_ITEM_SPEED] = 1u << SIG_OBD_SPEED,
        [DISP_ITEM_BAT] = 1u << SIG_OBD_BATTERY_VOLTAGE,
        [DISP_ITEM_OILP] = 1u << SIG_OBD_OIL_PRESSURE,
        [DISP_ITEM_BOOST] = 1u << SIG_OBD_BOOST,
        [DISP_ITEM_AFR] = 1u << SIG_OBD_AFR
    };
    return item < DISP_ITEM_COUNT ? masks[item] : 0;
}

// The Temp/Info/Chart pages historically wrap persisted indices modulo COUNT.
static inline uint16_t ui_data_mask_for_map(const uint8_t *map, size_t count)
{
    uint16_t mask = 0;
    for (size_t i = 0; i < count; i++)
        mask |= ui_data_mask_for_item(map[i] % DISP_ITEM_COUNT);
    return mask;
}
