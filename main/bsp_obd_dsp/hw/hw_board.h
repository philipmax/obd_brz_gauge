#pragma once
/*
 * hw_board.h - selects the board profile for the current build.
 *
 * Every hardware-specific value (LCD pins/clock, touch pins, optional chips
 * and the ST77916 init table) lives in exactly one of:
 *   hw_v1_waveshare.h  V1: Waveshare ESP32-S3-Touch-LCD-1.85
 *   hw_v2_new.h        V2: new board A (ST77916 v1 init)
 *   hw_v3_new.h        V3: new board B (ST77916 v2 init)
 *
 * Consumers keep using the historical macro names (ESP_PANEL_LCD_*, I2C_Touch_*,
 * I2C_SCL_IO ...); the board headers define HW_* and the consuming headers map
 * them, so only this directory needs to know about hardware revisions.
 */

#include "sdkconfig.h"

#if CONFIG_OBD_HW_VERSION_V2_NEW
#include "bsp_obd_dsp/hw/hw_v2_new.h"
#elif CONFIG_OBD_HW_VERSION_V3_NEW
#include "bsp_obd_dsp/hw/hw_v3_new.h"
#elif CONFIG_OBD_HW_VERSION_V1_WAVESHARE
#include "bsp_obd_dsp/hw/hw_v1_waveshare.h"
#else
#error "No OBD display hardware selected: menuconfig -> OBD DSP Configuration"
#endif
