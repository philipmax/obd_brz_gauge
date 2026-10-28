#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"
#define SD_MOUNT_POINT "/tmp/obd-upload-test"
bool sd_card_is_mounted(void);
size_t sd_card_write(FILE *, const void *, size_t);
esp_err_t sd_card_remove(const char *);
esp_err_t sd_card_rename(const char *, const char *);
