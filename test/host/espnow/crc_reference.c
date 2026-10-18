/* Frozen previous bitwise CRC-16/CCITT-FALSE; independent of SDK tables. */
#include <stdint.h>
#include <stddef.h>

uint16_t reference_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)(p[i] << 8));
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

