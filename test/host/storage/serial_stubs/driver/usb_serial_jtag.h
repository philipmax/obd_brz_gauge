#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef struct { size_t tx_buffer_size, rx_buffer_size; } usb_serial_jtag_driver_config_t;
bool usb_serial_jtag_is_driver_installed(void);
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config);
int usb_serial_jtag_read_bytes(void *dst, uint32_t len, TickType_t timeout);
int usb_serial_jtag_write_bytes(const void *src, size_t len, TickType_t timeout);
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t timeout);
