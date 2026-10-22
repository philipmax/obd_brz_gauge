#pragma once

#include <stddef.h>
#include <stdint.h>

#define RI_MAX_SIGNALS  48u
#define RI_MAX_SOURCES  96u
#define RI_MAX_REQUESTS 32u
#define RI_NONE         0xFFu

typedef struct {
    uint16_t first;
    uint8_t  count;
    uint8_t  cur;
    uint8_t  driven;
} ri_signal_t;

typedef struct {
    uint8_t kind;
    uint8_t ref;
    uint8_t sig;
} ri_source_t;

typedef struct {
    ri_signal_t sig[RI_MAX_SIGNALS];
    ri_source_t src[RI_MAX_SOURCES];
    uint8_t head[RI_MAX_REQUESTS];
    uint8_t next[RI_MAX_SOURCES];
    uint8_t n_sig;
    uint8_t n_src;
    uint8_t n_req;
} ri_case_t;

void ri_build_index(ri_case_t *c);
uint32_t ri_dispatch_linear(const ri_case_t *c, uint8_t request);
uint32_t ri_dispatch_indexed(const ri_case_t *c, uint8_t request);
