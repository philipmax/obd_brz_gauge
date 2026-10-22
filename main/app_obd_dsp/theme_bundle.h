#pragma once
#include "esp_err.h"
#include "bsp_obd_dsp/sd_card.h"

#define THEME_BUNDLE_PATH SD_DIR_IMPORT "/.usb-theme.bundle"
/* OB DT v1 stream, already SHA-256 verified by the serial upload sink. */
esp_err_t theme_bundle_install(const char *path);
/* Before loading a theme: roll back an interrupted directory swap. */
esp_err_t theme_bundle_recover(void);
