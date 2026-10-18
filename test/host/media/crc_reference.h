#pragma once
#include <stddef.h>
#include <stdint.h>

/* Previous firmware algorithms, kept in a separate TU for timing comparisons. */
uint32_t crc_bitwise_reference(uint32_t crc, const void *data, size_t len);
uint32_t crc_nibble_reference(uint32_t crc, const void *data, size_t len);
