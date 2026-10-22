#pragma once
#include "platform.h"
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 255
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 130
#define ESP_PARTITION_SUBTYPE_DATA_COREDUMP 3
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                               esp_partition_subtype_t subtype,
                                               const char *label);
esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *buf, size_t len);
