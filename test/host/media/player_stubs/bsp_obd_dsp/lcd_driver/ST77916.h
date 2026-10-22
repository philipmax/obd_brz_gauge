#pragma once
/* LCD boundary only: avoid importing the board's unrelated GPIO/I2C drivers. */
#include "esp_err.h"
#define EXAMPLE_LCD_WIDTH 360
#define EXAMPLE_LCD_HEIGHT 360
extern void *panel_handle;
esp_err_t esp_lcd_panel_draw_bitmap(void *panel, int x0, int y0, int x1, int y1,
                                   const void *pixels);
