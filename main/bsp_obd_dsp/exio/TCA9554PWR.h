#pragma once


#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"
#define TCA9554_EXIO1 0x01
#define TCA9554_EXIO2 0x02
#define TCA9554_EXIO3 0x03
#define TCA9554_EXIO4 0x04
#define TCA9554_EXIO5 0x05
#define TCA9554_EXIO6 0x06
#define TCA9554_EXIO7 0x07
#define TCA9554_EXIO8 0x08


/* TCA9554PWR register addresses used by the board drivers. */
#define TCA9554_ADDRESS       0x20
#define TCA9554_OUTPUT_REG    0x01
#define TCA9554_CONFIG_REG    0x03

/* Board-level EXIO operations used by the display and touch drivers. */
void Set_EXIO(uint8_t Pin, bool State);
esp_err_t EXIO_Init(void);
