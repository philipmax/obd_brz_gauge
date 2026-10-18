#pragma once
#include "platform.h"
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, uint32_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *data++;
        for (unsigned i=0; i<8; i++) crc=(crc>>1)^((crc&1) ? 0xedb88320u : 0);
    }
    return ~crc;
}
