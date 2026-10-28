#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_log.h"
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int clk_source, i2c_port, scl_io_num, sda_io_num, glitch_ignore_cnt; struct { int enable_internal_pullup; } flags; } i2c_master_bus_config_t;
typedef struct { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; } i2c_device_config_t;
typedef struct { const uint8_t *write_buffer; size_t buffer_size; } i2c_master_transmit_multi_buffer_info_t;
#define I2C_ADDR_BIT_7 7
#define I2C_CLK_SRC_DEFAULT 0
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *, i2c_master_bus_handle_t *);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t, i2c_master_transmit_multi_buffer_info_t *, size_t, int);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *, size_t, uint8_t *, size_t, int);
