#include "racechrono_filter.h"

#include <string.h>

static uint16_t interval_or_default(uint16_t interval_ms)
{
    return interval_ms ? interval_ms : 100;
}

void rc_filter_reset(rc_filter_t *filter)
{
    memset(filter, 0, sizeof(*filter));
    filter->allow_all = true;
    filter->all_interval_ms = 100;
}

void rc_filter_deny_all(rc_filter_t *filter)
{
    filter->allow_all = false;
    filter->subscribed_mask = 0;
}

void rc_filter_allow_all(rc_filter_t *filter, uint16_t interval_ms)
{
    filter->all_interval_ms = interval_or_default(interval_ms);
    filter->allow_all = true;
    filter->subscribed_mask = 0;
}

void rc_filter_subscribe(rc_filter_t *filter, unsigned channel, uint16_t interval_ms)
{
    if (channel >= RC_FILTER_CHANNEL_COUNT) return;
    uint16_t bit = (uint16_t)(1u << channel);
    if (!(filter->subscribed_mask & bit)) filter->last_sent_us[channel] = 0;
    filter->interval_ms[channel] = interval_or_default(interval_ms);
    filter->subscribed_mask |= bit;
}

uint16_t rc_filter_due_mask(const rc_filter_t *filter, uint16_t channels, int64_t now_us)
{
    uint16_t due = 0;
    channels &= RC_FILTER_CHANNEL_MASK;
    if (!filter->allow_all) channels &= filter->subscribed_mask;
    while (channels) {
        unsigned channel = (unsigned)__builtin_ctz((unsigned)channels);
        uint16_t bit = (uint16_t)(1u << channel);
        channels &= (uint16_t)~bit;
        uint16_t interval = filter->allow_all ? filter->all_interval_ms : filter->interval_ms[channel];
        if (interval < 20) interval = 20;
        int64_t last = filter->allow_all ? filter->last_all_sent_us[channel] : filter->last_sent_us[channel];
        if (now_us - last >= (int64_t)interval * 1000) due |= bit;
    }
    return due;
}

uint16_t rc_filter_claim_valid(rc_filter_t *filter, uint16_t valid, int64_t now_us)
{
    uint16_t due = rc_filter_due_mask(filter, valid, now_us);
    uint16_t remaining = due;
    int64_t *last = filter->allow_all ? filter->last_all_sent_us : filter->last_sent_us;
    while (remaining) {
        unsigned channel = (unsigned)__builtin_ctz((unsigned)remaining);
        remaining &= (uint16_t)~(1u << channel);
        last[channel] = now_us;
    }
    return due;
}

uint32_t rc_filter_next_wake_us(const rc_filter_t *filter, uint16_t channels,
                                int64_t now_us, int64_t sample_not_before_us)
{
    channels &= RC_FILTER_CHANNEL_MASK;
    if (!filter->allow_all) channels &= filter->subscribed_mask;
    if (!channels) return RC_FILTER_WAIT_FOREVER;

    uint64_t earliest = UINT64_MAX;
    while (channels) {
        unsigned channel = (unsigned)__builtin_ctz((unsigned)channels);
        channels &= (uint16_t)~(1u << channel);
        uint16_t interval = filter->allow_all ? filter->all_interval_ms : filter->interval_ms[channel];
        if (interval < 20) interval = 20;
        uint64_t period = (uint64_t)interval * 1000u;
        int64_t last = filter->allow_all ? filter->last_all_sent_us[channel] : filter->last_sent_us[channel];
        uint64_t remaining;
        if (now_us >= last) {
            uint64_t elapsed = (uint64_t)(now_us - last);
            remaining = elapsed >= period ? 0 : period - elapsed;
        } else {
            remaining = (uint64_t)(last - now_us) + period;
        }
        if (remaining < earliest) earliest = remaining;
    }
    uint64_t gate = sample_not_before_us > now_us ? (uint64_t)(sample_not_before_us - now_us) : 0;
    if (earliest < gate) earliest = gate;
    /* A finite long wait must never turn into an indefinite wait. */
    if (earliest >= RC_FILTER_WAIT_FOREVER) earliest = RC_FILTER_WAIT_FOREVER - 1u;
    return (uint32_t)earliest;
}
