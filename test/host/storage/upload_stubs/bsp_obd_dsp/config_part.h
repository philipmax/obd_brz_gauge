#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#define CONFIG_MOUNT_POINT "/tmp/obd-upload-config"
bool config_part_is_mounted(void);
bool config_part_has_quota(size_t);
esp_err_t config_part_write(const char *, const void *, size_t);
