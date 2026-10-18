#pragma once
#include "sdmmc_cmd.h"
#define GPIO_NUM_14 14
#define GPIO_NUM_17 17
#define GPIO_NUM_16 16
#define GPIO_NUM_NC (-1)
#define SDMMC_FREQ_HIGHSPEED 40000u
#define SDMMC_FREQ_DEFAULT 20000u
#define SDMMC_HOST_FLAG_1BIT 1u
#define SDMMC_HOST_FLAG_DEINIT_ARG 2u
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1u
typedef struct {
    int width, clk, cmd, d0, d1, d2, d3, d4, d5, d6, d7;
    uint32_t flags;
} sdmmc_slot_config_t;
esp_err_t sdmmc_host_init(void);
esp_err_t sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t *config);
esp_err_t sdmmc_host_deinit(void);
#define SDMMC_HOST_DEFAULT() { .slot = 1, .init = sdmmc_host_init }
#define SDMMC_SLOT_CONFIG_DEFAULT() { 0 }
