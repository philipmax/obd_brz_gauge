#pragma once
#include <stdint.h>
#include <stddef.h>
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, size_t len);
