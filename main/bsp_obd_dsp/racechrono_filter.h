#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pure scheduling state. The GATT wrapper serializes access with its own
 * lock; signal snapshots and BLE operations always happen outside that lock. */
#define RC_FILTER_CHANNEL_COUNT 9
#define RC_FILTER_CHANNEL_MASK ((uint16_t)((1u << RC_FILTER_CHANNEL_COUNT) - 1u))
#define RC_FILTER_INITIALIZER { .allow_all = true, .all_interval_ms = 100 }

typedef struct {
    bool allow_all;
    uint16_t all_interval_ms;
    uint16_t subscribed_mask;
    uint16_t interval_ms[RC_FILTER_CHANNEL_COUNT];
    int64_t last_sent_us[RC_FILTER_CHANNEL_COUNT];
    int64_t last_all_sent_us[RC_FILTER_CHANNEL_COUNT];
} rc_filter_t;

void rc_filter_reset(rc_filter_t *filter);
void rc_filter_deny_all(rc_filter_t *filter);
void rc_filter_allow_all(rc_filter_t *filter, uint16_t interval_ms);
void rc_filter_subscribe(rc_filter_t *filter, unsigned channel, uint16_t interval_ms);
/* Read-only: an invalid/unavailable sample must not consume its interval. */
uint16_t rc_filter_due_mask(const rc_filter_t *filter, uint16_t channels, int64_t now_us);
/* Recheck the latest filter and claim only valid, due samples before sending. */
uint16_t rc_filter_claim_valid(rc_filter_t *filter, uint16_t valid, int64_t now_us);

/* Read-only wake planning on the same monotonic, nonnegative clock as due_mask.
 * UINT32_MAX means no eligible channel: wait for a filter/connection event.
 * sample_not_before_us throttles retries of due but unavailable signals; it
 * does NOT claim their intervals. Finite waits saturate below the sentinel. */
#define RC_FILTER_WAIT_FOREVER UINT32_MAX
uint32_t rc_filter_next_wake_us(const rc_filter_t *filter, uint16_t channels,
                                int64_t now_us, int64_t sample_not_before_us);
