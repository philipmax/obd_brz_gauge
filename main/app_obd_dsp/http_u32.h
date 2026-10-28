#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Strict unsigned decimal parsing, independent of the target's long width. */
static inline bool http_parse_u32(const char *s, uint32_t *out)
{
    if (!s || !*s || !out) return false;
    uint32_t value = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        unsigned digit = (unsigned)(*s - '0');
        if (value > (UINT32_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}
