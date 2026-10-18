#ifndef RX_APPEND_PROBE_H
#define RX_APPEND_PROBE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct ra_case ra_case_t;
typedef struct {
    size_t used;
    bool prompt, refused;
    uint32_t frames, writes, clocks;
    int32_t coolant, oil;
} ra_observation_t;
ra_case_t *ra_create(void);
void ra_destroy(ra_case_t *c);
void ra_reset(ra_case_t *c, const uint8_t *prefix, size_t len,
              unsigned mode, bool prompt, uint32_t seed);
bool ra_compare(ra_case_t *c, const uint8_t *data, size_t len);
void ra_observe(const ra_case_t *c, ra_observation_t *out);
uint32_t ra_feed(ra_case_t *c, const uint8_t *data, size_t len);
size_t ra_engine_size(void);
#endif
