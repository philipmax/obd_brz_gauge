/* Desktop lookup/apply timing. FreeRTOS locks are stubs; no radio or production store writes. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app_obd_dsp/signal_store.h"
#include "bsp_obd_dsp/espnow_v6.h"

int lookup_reference(uint16_t num_id);
void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{ (void)caps; return calloc(n, size); }
int64_t esp_timer_get_time(void) { return 0; }

static volatile uint32_t sink;
static uint8_t bytes[ESPNOW_V6_MAX_FRAGS][ESPNOW_FRAME_MAX];
static espnow_rx_frame_t frames[ESPNOW_V6_MAX_FRAGS];
static int n_frames;
static uint16_t ids[64];
static unsigned n_ids;

typedef struct {
    int32_t values[SIG_STD_COUNT + SIG_VEH_MAX];
    bool valid[SIG_STD_COUNT + SIG_VEH_MAX];
    unsigned sets, invalidates;
} apply_state_t;

static int slot_old(void *ctx, uint16_t id)
{ (void)ctx; return lookup_reference(id); }
static int slot_new(void *ctx, uint16_t id)
{ (void)ctx; return signal_store_slot_by_id(id); }
static void set(void *ctx, int slot, int32_t value)
{
    apply_state_t *s = ctx;
    s->values[slot] = value; s->valid[slot] = true; s->sets++;
}
static void invalidate(void *ctx, int slot)
{
    apply_state_t *s = ctx;
    s->valid[slot] = false; s->invalidates++;
}
static void set_many(void *ctx, const int *slots, const int32_t *values, size_t count)
{ for (size_t i = 0; i < count; i++) set(ctx, slots[i], values[i]); }
static void invalidate_many(void *ctx, const int *slots, size_t count)
{ for (size_t i = 0; i < count; i++) invalidate(ctx, slots[i]); }

static espnow_apply_ops_t ops(apply_state_t *s, bool old)
{
    espnow_apply_ops_t o = { .ctx = s, .slot_by_id = old ? slot_old : slot_new,
        .set = set, .invalidate = invalidate, .set_many = set_many,
        .invalidate_many = invalidate_many };
    return o;
}
static void state_init(apply_state_t *s)
{
    memset(s, 0, sizeof(*s));
    for (unsigned i = 0; i < sizeof(s->valid) / sizeof(s->valid[0]); i++) {
        s->valid[i] = true; s->values[i] = -123456;
    }
}
static double now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static double sample(bool apply, bool old, bool veh_ok)
{
    apply_state_t state;
    state_init(&state);
    espnow_apply_ops_t o = ops(&state, old);
    int (*lookup)(uint16_t) = old ? lookup_reference : signal_store_slot_by_id;
    uint32_t sum = 0;
    uint64_t calls = 0;
    double start = now(), elapsed;
    do {
        for (unsigned i = 0; i < 128; i++) {
            if (apply) {
                for (int f = 0; f < n_frames; f++)
                    sum += (uint32_t)espnow_rx_apply(&frames[f], veh_ok, &o);
            } else {
                for (unsigned j = 0; j < n_ids; j++) sum += (uint32_t)lookup(ids[j]);
            }
        }
        calls += apply ? 128 : 128 * n_ids;
        elapsed = now() - start;
    } while (elapsed < 0.020);
    sink ^= sum + state.sets + state.invalidates + (uint32_t)state.values[0];
    return elapsed * 1e9 / (double)calls;
}
static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void row(const char *name, bool apply, int registered, bool veh_ok)
{
    if (apply) {
        apply_state_t a, b;
        state_init(&a); state_init(&b);
        espnow_apply_ops_t old = ops(&a, true), next = ops(&b, false);
        for (int f = 0; f < n_frames; f++) {
            int x = espnow_rx_apply(&frames[f], veh_ok, &old);
            int y = espnow_rx_apply(&frames[f], veh_ok, &next);
            if (x != y || memcmp(&a, &b, sizeof(a))) abort();
        }
    } else {
        for (unsigned i = 0; i < n_ids; i++)
            if (lookup_reference(ids[i]) != signal_store_slot_by_id(ids[i])) abort();
    }
    double old[7], next[7], sorted[7];
    for (unsigned group = 0; group < 7; group++) {
        if (group % 2) {
            next[group] = sample(apply, false, veh_ok); old[group] = sample(apply, true, veh_ok);
        } else {
            old[group] = sample(apply, true, veh_ok); next[group] = sample(apply, false, veh_ok);
        }
    }
    memcpy(sorted, old, sizeof(old)); qsort(sorted, 7, sizeof(double), cmp); double om = sorted[3];
    memcpy(sorted, next, sizeof(next)); qsort(sorted, 7, sizeof(double), cmp); double nm = sorted[3];
    printf("%s,%s,%d,%d,%d,%.6f,%.6f,%.6f,\"", apply ? "apply" : "lookup", name,
        registered, apply ? n_frames : 0, veh_ok, om, nm, nm / om);
    for (unsigned i = 0; i < 7; i++) printf("%s%.6f", i ? ";" : "", old[i]);
    printf("\",\"");
    for (unsigned i = 0; i < 7; i++) printf("%s%.6f", i ? ";" : "", next[i]);
    puts("\"");
}
static void registered(int n)
{
    signal_store_veh_clear();
    for (int i = 0; i < n; i++) {
        char key[32]; snprintf(key, sizeof(key), "veh.bench_%d", i);
        if (signal_store_veh_register(key, -1000000, 1000000, 5000) != SIG_VEH_BASE + i) abort();
    }
}
static void prepare(int standard, int dynamic, int unknown, bool all_valid, bool v5)
{
    espnow_snapshot_t s;
    espnow_snapshot_init(&s);
    s.seq = 0x123456; memcpy(s.name, "Lookup", 7);
    if (standard) {
        for (int id = ESPNOW_ID_STD_FIRST; id <= ESPNOW_ID_LOAD; id++)
            if (!espnow_snapshot_put(&s, (uint16_t)id, id == ESPNOW_ID_GEAR ? 3 : id * 37)) abort();
    }
    for (int i = 0; i < dynamic; i++)
        if (!espnow_snapshot_put(&s, (uint16_t)(ESPNOW_ID_VEH_FIRST + i), -i * 73)) abort();
    for (int i = 0; i < unknown; i++)
        if (!espnow_snapshot_put(&s, (uint16_t)(0x0500 + i), i * 19)) abort();
    if (all_valid) { s.std_valid = UINT32_MAX; s.veh_valid = UINT32_MAX; }
    n_frames = v5 ? 1 : espnow_v6_frame_count(&s);
    for (int f = 0; f < n_frames; f++) {
        size_t n = v5 ? espnow_v5_encode(&s, bytes[f], sizeof(bytes[f])) :
                       espnow_v6_encode(&s, f, bytes[f], sizeof(bytes[f]));
        if (!n || !(v5 ? espnow_v5_decode(bytes[f], n, &frames[f]) :
                         espnow_v6_decode(bytes[f], n, &frames[f]))) abort();
    }
}
int main(void)
{
    puts("operation,case,registered,fragments,veh_ok,old_ns,new_ns,new_over_old,old_samples_ns,new_samples_ns");
    ids[0] = SIG_VEH_ID_BASE; n_ids = 1;
    row("dynamic_before_init", false, 0, false);
    signal_store_init(); registered(SIG_VEH_MAX);
    static const struct { const char *name; uint16_t id; } cases[] = {
        {"rpm", 0x0001}, {"gear", 0x0006}, {"load", 0x000C},
        {"sys_first", 0x0080}, {"sys_last", 0x0084}, {"static_hole", 0x000D},
        {"unknown", 0xFFFF}, {"dynamic_first", 0x0100}, {"dynamic_last", 0x011F}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ids[0] = cases[i].id; n_ids = 1; row(cases[i].name, false, SIG_VEH_MAX, false);
    }
    n_ids = 0;
    for (int i = 0; i < SIG_STD_COUNT; i++) ids[n_ids++] = sig_registry[i].num_id;
    for (int i = 0; i < SIG_VEH_MAX; i++) ids[n_ids++] = (uint16_t)(SIG_VEH_ID_BASE + i);
    ids[n_ids++] = 0; ids[n_ids++] = 0xFFFF;
    row("mixed", false, SIG_VEH_MAX, false);
    registered(0); ids[0] = SIG_VEH_ID_BASE; n_ids = 1;
    row("dynamic_unregistered", false, 0, false);
    static const struct {
        const char *name; int n_reg, standard, dynamic, unknown; bool valid, veh_ok, v5;
    } scenarios[] = {
        {"empty", 0, 0, 0, 0, false, false, false},
        {"empty_all_valid", 0, 0, 0, 0, true, true, false},
        {"standard", 0, 1, 0, 0, false, false, false},
        {"standard_veh_ok", 0, 1, 0, 0, false, true, false},
        {"standard_invalidate32", 32, 1, 0, 0, false, true, false},
        {"standard_dynamic1", 1, 1, 1, 0, false, true, false},
        {"standard_dynamic32", 32, 1, 32, 0, false, true, false},
        {"empty_invalidate32", 32, 0, 0, 0, false, true, false},
        {"unknown24", 0, 0, 0, 24, false, false, false},
        {"full64", 32, 1, 32, 21, false, true, false},
        {"hash_mismatch", 32, 1, 32, 0, false, false, false},
        {"v5_standard", 0, 1, 0, 0, false, false, true}
    };
    for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        registered(scenarios[i].n_reg);
        prepare(scenarios[i].standard, scenarios[i].dynamic, scenarios[i].unknown,
                scenarios[i].valid, scenarios[i].v5);
        row(scenarios[i].name, true, scenarios[i].n_reg, scenarios[i].veh_ok);
    }
    return 0;
}
