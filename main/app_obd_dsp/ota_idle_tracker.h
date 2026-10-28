#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_obd_dsp/ota_wifi_server.h"

/* The caller serializes all access, including 64-bit timestamps. */
typedef struct {
    ota_wifi_state_t state;
    int64_t last_activity_us;
    uint32_t generation;
} ota_idle_tracker_t;

static inline uint32_t ota_idle_begin(ota_idle_tracker_t *t, int64_t now)
{
    if (++t->generation == 0) ++t->generation;
    t->last_activity_us = now;
    t->state = OTA_WIFI_STATE_STARTING;
    return t->generation;
}

static inline void ota_idle_state(ota_idle_tracker_t *t, ota_wifi_state_t state, int64_t now)
{
    t->state = state;
    if (now > t->last_activity_us) t->last_activity_us = now;
}

static inline bool ota_idle_current(const ota_idle_tracker_t *t, uint32_t generation)
{
    return generation == t->generation && t->state != OTA_WIFI_STATE_IDLE;
}

static inline bool ota_idle_expired(const ota_idle_tracker_t *t, uint32_t generation,
                                    int64_t now, int64_t timeout)
{
    return ota_idle_current(t, generation) && now >= t->last_activity_us &&
           now - t->last_activity_us > timeout;
}
