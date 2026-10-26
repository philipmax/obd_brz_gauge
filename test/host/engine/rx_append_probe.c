/* Real private RX state plus an independent pre-optimization reference.
 * Overrides and symbol aliases are host-only; firmware has no probe hooks. */
#include "rx_append_probe.h"
#ifndef RA_ENGINE_SOURCE
#define RA_ENGINE_SOURCE "../../../main/vehicle/vp_engine.c"
#endif
#include RA_ENGINE_SOURCE

#define RA_EVENTS 256u
struct ra_event { uint32_t kind; int32_t slot, value; };
struct ra_trace {
    unsigned n, clocks, writes;
    int32_t coolant, oil;
    struct ra_event events[RA_EVENTS];
};
struct ra_case {
    vp_engine_t e, initial, before, expected;
    vp_profile_t *profile;
    struct ra_trace traces[2];
    unsigned active;
    uint32_t now;
};
static void event(ra_case_t *c, uint32_t kind, int slot, int32_t value)
{
    struct ra_trace *t = &c->traces[c->active];
    if (t->n >= RA_EVENTS) abort();
    t->events[t->n].kind = kind;
    t->events[t->n].slot = slot;
    t->events[t->n++].value = value;
}
static bool send_cmd(void *ctx, const char *cmd)
{
    (void)cmd; event(ctx, 1, 0, 0); return true;
}
static uint32_t now_ms(void *ctx)
{
    ra_case_t *c = ctx;
    c->traces[c->active].clocks++;
    return c->now;
}
static void set_signal(void *ctx, int slot, int32_t value)
{
    ra_case_t *c = ctx;
    struct ra_trace *t = &c->traces[c->active];
    t->writes++;
    if (slot == SIG_OBD_COOLANT_TEMP) t->coolant = value;
    if (slot == SIG_OBD_OIL_TEMP) t->oil = value;
    event(c, 2, slot, value);
}
static void invalidate(void *ctx, int slot) { event(ctx, 3, slot, 0); }
static void speed(void *ctx, uint8_t value) { event(ctx, 4, 0, value); }
static void link_state(void *ctx, uint8_t value) { event(ctx, 5, 0, value); }
static void response(void *ctx, int slot) { event(ctx, 6, slot, 0); }
static void *heap_alloc(void *ctx, size_t len) { (void)ctx; return malloc(len); }
static void heap_free(void *ctx, void *p) { (void)ctx; free(p); }
ra_case_t *ra_create(void)
{
    static const char json[] =
        "{\"schema\":\"skygauge.vehicle/1\",\"id\":\"test.rx\",\"rev\":1,\"name\":\"RX\","
        "\"defaults\":\"none\",\"bus\":{\"proto\":6,\"gap_ms\":1},"
        "\"requires\":{\"elm\":[\"ATMA\"]},"
        "\"monitor\":{\"mode\":\"probe\",\"every_ms\":500,\"window_ms\":120,\"stale_ms\":15000},"
        "\"signals\":[{\"id\":\"obd.coolant_temp\",\"src\":["
        "{\"can\":\"360\",\"bit\":24,\"bits\":8,\"rpn\":[[\"X\"],[\"k\",40],\"-\"]},"
        "{\"req\":\"0105\",\"len\":1,\"rpn\":[[\"A\",0],[\"k\",40],\"-\"]}]},"
        "{\"id\":\"obd.oil_temp\",\"src\":["
        "{\"can\":\"360\",\"bit\":16,\"bits\":8,\"rpn\":[[\"X\"],[\"k\",40],\"-\"]}]}]}";
    vp_allocator_t a = { .alloc = heap_alloc, .free = heap_free, .ctx = NULL };
    vp_engine_sink_t sink;
    vp_engine_t *e;
    vp_issue_t items[16]; vp_issues_t issues;
    ra_case_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    vp_issues_init(&issues, items, 16);
    if (vp_compile_json(json, sizeof(json)-1, vp_validate_default_cfg(), &a,
                        &c->profile, &issues) != VP_COMPILE_OK) {
        unsigned i;
        for (i=0; i<issues.count; i++) fprintf(stderr, "%s\n", items[i].msg);
        free(c); return NULL;
    }
    memset(&sink, 0, sizeof(sink)); sink.ctx = c;
    sink.send = send_cmd; sink.now_ms = now_ms; sink.set_signal = set_signal;
    sink.invalidate = invalidate; sink.set_speed_kmh = speed; sink.on_link = link_state;
    sink.on_response = response;
    e = vp_engine_create(c->profile, &sink, NULL, &a);
    if (!e) { free(c->profile); free(c); return NULL; }
    c->initial = *e;
    vp_engine_destroy(e);
    c->e = c->initial;
    return c;
}
void ra_destroy(ra_case_t *c)
{
    if (c) { free(c->profile); free(c); }
}
void ra_reset(ra_case_t *c, const uint8_t *prefix, size_t len,
              unsigned mode, bool prompt, uint32_t seed)
{
    size_t i;
    if (!c || len > ENG_RX_CAP || mode > 4 || (len && !prefix)) abort();
    c->e = c->initial;
    c->now = seed;
    c->active = 0;
    memset(c->traces, 0, sizeof(c->traces));
    c->e.busy = mode == 1 || mode == 2 || mode == 3;
    c->e.cur.kind = mode == 2 || mode == 4 ? K_MA : mode == 3 ? K_RV : K_REQ;
    c->e.prompt = prompt;
    c->e.rx_len = (uint16_t)len;
    /* Fill unused bytes too: compare the entire engine, including tails. */
    for (i=0; i<sizeof(c->e.rxbuf); i++) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        c->e.rxbuf[i] = (char)(seed >> 24);
    }
    if (len) memcpy(c->e.rxbuf, prefix, len);
    c->e.rxbuf[len] = 0;
    c->e.sigs[0].cur = (uint8_t)(seed & 1u); /* CAN promotion from request fallback. */
    c->e.last_data_ms = seed - 1;
    c->e.last_good_ms = seed - 100;
}
/* Saved behavior, intentionally separate from the production implementation. */
static void reference_rx(vp_engine_t *e, const uint8_t *data, size_t len)
{
    size_t i;
    bool line = false;
    if (!e || !data) return;
    for (i=0; i<len; i++) {
        char ch = (char)data[i];
        if (ch == 0) continue;
        if (ch == '>') e->prompt = true;
        if (ch == '\r' || ch == '\n' || ch == '>') line = true;
        if (e->rx_len < ENG_RX_CAP) e->rxbuf[e->rx_len++] = ch;
    }
    e->rxbuf[e->rx_len] = 0;
    if (e->busy && e->cur.kind == K_MA && (line || e->rx_len >= ENG_RX_CAP)) {
        mon_consume(e, e->sink.now_ms(e->sink.ctx), false);
        e->rxbuf[e->rx_len] = 0;
    }
}
bool ra_compare(ra_case_t *c, const uint8_t *data, size_t len)
{
    if (!c) { reference_rx(NULL, data, len); vp_engine_on_rx(NULL, data, len); return true; }
    c->before = c->e;
    memset(c->traces, 0, sizeof(c->traces));
    c->active = 0;
    reference_rx(&c->e, data, len);
    c->expected = c->e;
    c->e = c->before;
    c->active = 1;
    vp_engine_on_rx(&c->e, data, len);
    if (memcmp(&c->e, &c->expected, sizeof(c->e)) ||
        memcmp(&c->traces[0], &c->traces[1], sizeof(c->traces[0]))) return false;
    c->now++;
    return true;
}
void ra_observe(const ra_case_t *c, ra_observation_t *out)
{
    const struct ra_trace *t = &c->traces[c->active];
    memset(out, 0, sizeof(*out));
    out->used = c->e.rx_len; out->prompt = c->e.prompt; out->refused = c->e.ma_refused;
    out->frames = c->e.st.can_frames; out->writes = t->writes; out->clocks = t->clocks;
    out->coolant = t->coolant; out->oil = t->oil;
}
uint32_t ra_feed(ra_case_t *c, const uint8_t *data, size_t len)
{
    /* Timing uses a fixed address, resets only fields touched by RX, and reads
     * a result. No state copy/checksum over the entire engine inside the loop. */
    c->e.rx_len = 0;
    c->e.prompt = false;
    vp_engine_on_rx(&c->e, data, len);
    return c->e.rx_len + (uint32_t)c->e.prompt;
}
size_t ra_engine_size(void) { return sizeof(vp_engine_t); }
size_t ra_rx_capacity(void) { return ENG_RX_CAP; }
