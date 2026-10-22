/* Run the actual scheduling core against the previous rule-list semantics. */
#include "racechrono_filter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)

typedef struct {
    bool all;
    uint16_t interval;
    unsigned count;
    struct { unsigned channel; uint16_t interval; int64_t last; } rules[RC_FILTER_CHANNEL_COUNT];
    int64_t last_all[RC_FILTER_CHANNEL_COUNT];
} reference_t;

static void reference_reset(reference_t *ref)
{
    memset(ref, 0, sizeof(*ref));
    ref->all = true;
    ref->interval = 100;
}

static void reference_write(reference_t *ref, unsigned command, unsigned channel, uint16_t interval)
{
    if (command == 0) { ref->all = false; ref->count = 0; return; }
    if (command == 1) { ref->all = true; ref->count = 0; ref->interval = interval ? interval : 100; return; }
    if (channel >= RC_FILTER_CHANNEL_COUNT) return;
    for (unsigned i = 0; i < ref->count; i++) {
        if (ref->rules[i].channel == channel) {
            ref->rules[i].interval = interval ? interval : 100;
            return;
        }
    }
    unsigned i = ref->count++;
    ref->rules[i].channel = channel;
    ref->rules[i].interval = interval ? interval : 100;
    ref->rules[i].last = 0;
}

static uint16_t reference_send(reference_t *ref, uint16_t valid, int64_t now)
{
    uint16_t result = 0;
    for (unsigned channel = 0; channel < RC_FILTER_CHANNEL_COUNT; channel++) {
        if (!(valid & (1u << channel))) continue;
        uint16_t interval = ref->interval;
        int64_t *last = &ref->last_all[channel];
        if (!ref->all) {
            unsigned i;
            for (i = 0; i < ref->count; i++) if (ref->rules[i].channel == channel) break;
            if (i == ref->count) continue;
            interval = ref->rules[i].interval;
            last = &ref->rules[i].last;
        }
        if (interval < 20) interval = 20;
        if (now - *last < (int64_t)interval * 1000) continue;
        *last = now;
        result |= (uint16_t)(1u << channel);
    }
    return result;
}

static void write_both(rc_filter_t *filter, reference_t *ref, unsigned cmd, unsigned channel, uint16_t interval)
{
    if (cmd == 0) rc_filter_deny_all(filter);
    else if (cmd == 1) rc_filter_allow_all(filter, interval);
    else rc_filter_subscribe(filter, channel, interval);
    reference_write(ref, cmd, channel, interval);
}

/* Independent absolute-deadline oracle; production uses relative waits. */
static uint32_t reference_wait(const reference_t *ref, uint16_t channels,
                               int64_t now, int64_t gate)
{
    uint64_t deadline = UINT64_MAX;
    for (unsigned ch = 0; ch < RC_FILTER_CHANNEL_COUNT; ch++) {
        if (!(channels & (1u << ch))) continue;
        uint16_t interval = ref->interval;
        int64_t last = ref->last_all[ch];
        if (!ref->all) {
            unsigned i;
            for (i = 0; i < ref->count && ref->rules[i].channel != ch; i++) {}
            if (i == ref->count) continue;
            interval = ref->rules[i].interval;
            last = ref->rules[i].last;
        }
        uint64_t at = (uint64_t)last + (uint64_t)(interval < 20 ? 20 : interval) * 1000u;
        if (at < deadline) deadline = at;
    }
    if (deadline == UINT64_MAX) return RC_FILTER_WAIT_FOREVER;
    if (deadline < (uint64_t)gate) deadline = (uint64_t)gate;
    uint64_t remaining = deadline > (uint64_t)now ? deadline - (uint64_t)now : 0;
    return remaining >= UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)remaining;
}

static void test_randomized_equivalence(void)
{
    rc_filter_t filter = RC_FILTER_INITIALIZER;
    reference_t ref;
    reference_reset(&ref);
    uint32_t seed = 0x91365A77u;
    int64_t now = 0;
    for (unsigned step = 0; step < 20000; step++) {
        seed = seed * 1664525u + 1013904223u;
        now += seed % 40001;
        if (seed & 0x80) {
            unsigned cmd = (seed >> 24) % 3;
            unsigned channel = (seed >> 16) % (RC_FILTER_CHANNEL_COUNT + 1);
            uint16_t interval = step % 7 == 0 ? 0 : (uint16_t)(seed % 201);
            write_both(&filter, &ref, cmd, channel, interval);
        }
        uint16_t available = (uint16_t)(seed & RC_FILTER_CHANNEL_MASK);
        rc_filter_t before = filter;
        int64_t gate = step % 3 == 0 ? now + (seed % 40000) : now;
        CHECK(rc_filter_next_wake_us(&filter, (uint16_t)(available | 0xF000u), now, gate) ==
              reference_wait(&ref, available, now, gate));
        uint16_t due = rc_filter_due_mask(&filter, available, now);
        CHECK(memcmp(&filter, &before, sizeof(filter)) == 0); /* planning never claims */
        reference_t preview = ref;
        CHECK(due == reference_send(&preview, available, now));
        /* A concurrent GATT write between snapshot and claim is rechecked. */
        if (step % 11 == 0) write_both(&filter, &ref, 0, 0, 0);
        if (step % 17 == 0) write_both(&filter, &ref, 2, step % RC_FILTER_CHANNEL_COUNT, 20);
        uint16_t valid = (uint16_t)(due & (seed >> 9));
        CHECK(rc_filter_claim_valid(&filter, valid, now) == reference_send(&ref, valid, now));
        for (unsigned channel = 0; channel < RC_FILTER_CHANNEL_COUNT; channel++)
            CHECK(filter.last_all_sent_us[channel] == ref.last_all[channel]);
        for (unsigned i = 0; i < ref.count; i++)
            CHECK(filter.last_sent_us[ref.rules[i].channel] == ref.rules[i].last);
        if (step % 199 == 0) { rc_filter_reset(&filter); reference_reset(&ref); }
    }
}

static void test_invalid_and_intervals(void)
{
    rc_filter_t filter = RC_FILTER_INITIALIZER;
    CHECK(rc_filter_due_mask(&filter, 1, 99999) == 0);
    CHECK(rc_filter_due_mask(&filter, 1, 100000) == 1);
    CHECK(rc_filter_claim_valid(&filter, 0, 100000) == 0);
    CHECK(filter.last_all_sent_us[0] == 0);
    CHECK(rc_filter_claim_valid(&filter, 1, 120000) == 1);
    CHECK(rc_filter_claim_valid(&filter, 1, 200000) == 0);
    CHECK(rc_filter_claim_valid(&filter, 1, 220000) == 1);
    rc_filter_deny_all(&filter);
    CHECK(rc_filter_due_mask(&filter, UINT16_MAX, 1000000) == 0);
    rc_filter_subscribe(&filter, 0, 1);
    CHECK(rc_filter_claim_valid(&filter, 1, 20000) == 1); /* minimum 20 ms */
    CHECK(rc_filter_claim_valid(&filter, 1, 39999) == 0);
    CHECK(rc_filter_claim_valid(&filter, 1, 40000) == 1);
    rc_filter_subscribe(&filter, 0, 100);
    CHECK(rc_filter_claim_valid(&filter, 1, 100000) == 0); /* update keeps timestamp */
    CHECK(rc_filter_claim_valid(&filter, 1, 140000) == 1);
    rc_filter_deny_all(&filter);
    rc_filter_subscribe(&filter, 0, 0); /* re-add starts from zero; 0 means 100 ms */
    CHECK(rc_filter_claim_valid(&filter, 1, 100000) == 1);
    rc_filter_allow_all(&filter, 0);
    CHECK(rc_filter_claim_valid(&filter, 1, 300000) == 0); /* all-mode history retained */
    CHECK(rc_filter_claim_valid(&filter, 1, 320000) == 1);
    rc_filter_reset(&filter);
    CHECK(rc_filter_claim_valid(&filter, 1, 100000) == 1); /* new connection resets history */
    CHECK(rc_filter_due_mask(&filter, UINT16_MAX, 200000) == RC_FILTER_CHANNEL_MASK);
}

static void measure_snapshot_work(unsigned mode)
{
    rc_filter_t filter = RC_FILTER_INITIALIZER;
    if (mode) rc_filter_deny_all(&filter);
    if (mode == 1) rc_filter_subscribe(&filter, 0, 100);
    unsigned batches = 0, values = 0, sent = 0;
    /* Eight live channels; F109's reserved slot is never sampled. */
    for (int64_t now = 20000; now <= 1000000; now += 20000) {
        uint16_t due = rc_filter_due_mask(&filter, 0xFF, now);
        if (due) batches++;
        values += (unsigned)__builtin_popcount((unsigned)due);
        sent += (unsigned)__builtin_popcount((unsigned)rc_filter_claim_valid(&filter, due, now));
    }
    CHECK(batches == (mode == 2 ? 0 : 10));
    CHECK(values == (mode == 0 ? 80 : mode == 1 ? 10 : 0));
    CHECK(sent == values);
    printf("RaceChrono mode=%u ticks=50 snapshots=%u slot_reads=%u notifications=%u\n", mode, batches, values, sent);
}

static void test_wake_boundaries(void)
{
    rc_filter_t f = RC_FILTER_INITIALIZER;
    CHECK(rc_filter_next_wake_us(&f, 1, 0, 0) == 100000);
    CHECK(rc_filter_next_wake_us(&f, 1, 99999, 0) == 1);
    CHECK(rc_filter_next_wake_us(&f, 1, 100000, 0) == 0);
    CHECK(rc_filter_next_wake_us(&f, 0, 100000, INT64_MAX) == RC_FILTER_WAIT_FOREVER);
    CHECK(rc_filter_next_wake_us(&f, 0xF000, 100000, 0) == RC_FILTER_WAIT_FOREVER);
    CHECK(rc_filter_next_wake_us(&f, 1, 100000, 120000) == 20000);
    CHECK(rc_filter_claim_valid(&f, 0, 100000) == 0);
    CHECK(rc_filter_next_wake_us(&f, 1, 120000, 120000) == 0);
    CHECK(rc_filter_claim_valid(&f, 1, 120000) == 1);
    CHECK(rc_filter_next_wake_us(&f, 1, 120000, 140000) == 100000);
    rc_filter_allow_all(&f, 1);
    CHECK(rc_filter_next_wake_us(&f, 1, 139999, 0) == 1);
    CHECK(rc_filter_next_wake_us(&f, 1, 140000, 0) == 0);
    rc_filter_deny_all(&f);
    CHECK(rc_filter_next_wake_us(&f, 0xFF, 140000, 0) == RC_FILTER_WAIT_FOREVER);
    rc_filter_subscribe(&f, 8, 1); /* F109 may be subscribed, but is not a live slot. */
    CHECK(rc_filter_next_wake_us(&f, 0xFF, 140000, 0) == RC_FILTER_WAIT_FOREVER);
    rc_filter_subscribe(&f, 0, 65535);
    CHECK(rc_filter_next_wake_us(&f, 1, 0, 0) == 65535000);
    CHECK(rc_filter_next_wake_us(&f, 1, 65534999, 0) == 1);
    CHECK(rc_filter_claim_valid(&f, 1, 65535000) == 1);
    rc_filter_subscribe(&f, 0, 0); /* Updating interval retains individual history. */
    CHECK(rc_filter_next_wake_us(&f, 1, 65535000, 0) == 100000);
    rc_filter_allow_all(&f, 100); /* Separate all-mode history is still 120000. */
    CHECK(rc_filter_next_wake_us(&f, 1, 219999, 0) == 1);
    rc_filter_deny_all(&f);
    rc_filter_subscribe(&f, 0, 0); /* Re-add resets history. */
    CHECK(rc_filter_next_wake_us(&f, 1, 100000, 0) == 0);
    CHECK(rc_filter_next_wake_us(&f, 1, 0, INT64_MAX) == UINT32_MAX - 1u);
    f.last_sent_us[0] = INT64_MAX;
    CHECK(rc_filter_next_wake_us(&f, 1, 0, 0) == UINT32_MAX - 1u);
    CHECK(rc_filter_next_wake_us(&f, 1, INT64_MAX, INT64_MAX) == 100000);
    CHECK(rc_filter_next_wake_us(&f, 1, INT64_MAX - 3, 0) == 100003);
    f.last_sent_us[0] = INT64_MAX - 100000;
    CHECK(rc_filter_next_wake_us(&f, 1, INT64_MAX, 0) == 0);
    rc_filter_reset(&f);
    CHECK(rc_filter_next_wake_us(&f, 1, 0, 0) == 100000);
}

/* Deterministic microsecond model, not an RTOS or radio timing measurement. */
static void measure_wakes(unsigned mode)
{
    rc_filter_t f = RC_FILTER_INITIALIZER;
    if (mode == 1 || mode == 2) rc_filter_deny_all(&f);
    if (mode == 1) rc_filter_subscribe(&f, 0, 100);
    if (mode == 4) rc_filter_allow_all(&f, 101);
    int64_t now = 0, gate = 0;
    unsigned wakes = 0, batches = 0, sent = 0;
    while (true) {
        uint32_t wait = rc_filter_next_wake_us(&f, 0xFF, now, gate);
        if (wait == RC_FILTER_WAIT_FOREVER || now + wait > 1000000) break;
        CHECK(wait > 0); /* no busy retry */
        now += wait;
        wakes++;
        uint16_t due = rc_filter_due_mask(&f, 0xFF, now);
        CHECK(due != 0);
        if (due) { batches++; gate = now + 20000; }
        sent += (unsigned)__builtin_popcount((unsigned)rc_filter_claim_valid(&f, mode == 3 ? 0 : due, now));
    }
    CHECK(wakes == batches);
    CHECK(wakes == (mode == 2 ? 0 : mode == 3 ? 46 : mode == 4 ? 9 : 10));
    CHECK(sent == (mode == 0 ? 80 : mode == 1 ? 10 : mode == 4 ? 72 : 0));
    printf("deadline mode=%u wakes=%u snapshots=%u notifications=%u\n", mode, wakes, batches, sent);
}

int main(void)
{
    test_invalid_and_intervals();
    test_wake_boundaries();
    for (unsigned mode = 0; mode < 5; mode++) measure_wakes(mode);
    test_randomized_equivalence();
    for (unsigned mode = 0; mode < 3; mode++) measure_snapshot_work(mode);
    printf("racechrono_filter: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
