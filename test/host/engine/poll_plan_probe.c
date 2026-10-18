/* Compile the actual private planning functions, without firmware test hooks.
 * Linking engine_core does not pull its vp_engine.o: this TU defines the API.
 * Keep the independent pre-mask reference here, outside production code. */
#include "poll_plan_probe.h"
#include "../../../main/vehicle/vp_engine.c"

struct pp_case {
    vp_engine_t e;
    vp_profile_t p;
    vp_signal_t sig[VP_MAX_SIGNALS];
    vp_source_t src[VP_MAX_SOURCES];
};

static uint32_t rng(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

pp_case_t *pp_create(unsigned signals, unsigned pattern, uint32_t seed, bool mixed)
{
    pp_case_t *c;
    unsigned i;
    if (signals > VP_MAX_SIGNALS) return NULL;
    c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->p.n_sig = (uint16_t)signals;
    c->p.n_src = (uint16_t)(2 * signals);
    c->p.n_req = VP_MAX_REQUESTS;
    c->e.p = &c->p;
    c->e.sig = c->sig;
    c->e.src = c->src;
    c->e.round_no = rng(&seed);
    for (i = 0; i < signals; i++) {
        vp_signal_t *s = &c->sig[i];
        eng_sig_t *g = &c->e.sigs[i];
        unsigned k;
        s->src_first = (uint16_t)(2 * i);
        s->src_count = 2;
        s->rate = VP_RATE_ROUND;
        g->slot = (int16_t)i;
        for (k = 0; k < 2; k++) {
            vp_source_t *src = &c->src[2 * i + k];
            src->kind = VP_SRC_REQ;
            src->sig = (uint16_t)i;
            if (pattern == 0) src->ref = (uint16_t)((31u - i) % VP_MAX_REQUESTS);
            else if (pattern == 1) src->ref = 31; /* includes unsigned high bit */
            else if (pattern == 2) src->ref = (uint16_t)((i / 2 + k) % VP_MAX_REQUESTS);
            else src->ref = (uint16_t)(rng(&seed) % VP_MAX_REQUESTS);
        }
        if (mixed) {
            uint32_t r = rng(&seed);
            s->period_ms = i % 4 == 0 ? 0 : (uint16_t)(20 + (r >> 3) % 480);
            s->slow_div = i % 3 == 0 ? 0 : (uint8_t)(1 + (r >> 13) % 7);
            if (i % 5 == 0) s->rate = VP_RATE_SLOW;
            if (i < VP_MAX_EVERY_SIGNALS) s->rate = VP_RATE_EVERY;
            if (i % 11 == 3) g->slot = SIG_SLOT_NONE;
            if (i % 11 == 4) s->derive = VP_DERIVE_RATIO;
            if (i % 11 == 5) s->src_count = 0;
            c->src[2 * i + 1].kind = VP_SRC_CAN;
            g->cur = (uint8_t)((r >> 8) & 1);
            g->last_poll_ms = rng(&seed);
        }
        if (sig_driven(&c->e, (int)i)) {
            c->e.poll_sig[c->e.n_poll++] = (uint8_t)i;
            if (s->rate == VP_RATE_EVERY) c->e.every_sig[c->e.n_every++] = (uint8_t)i;
        }
    }
    return c;
}

void pp_destroy(pp_case_t *c) { free(c); }

static void reference_round(vp_engine_t *e, uint32_t now)
{
    uint8_t ev[VP_MAX_EVERY_SIGNALS], k;
    e->round_n = 0;
    e->round_pos = 0;
    e->every_done = false;
    for (k = 0; k < e->n_poll; k++) {
        int i = e->poll_sig[k];
        uint8_t r, j;
        if (!poll_candidate(e, i) || !sig_due(e, i, now)) continue;
        r = (uint8_t)cur_src(e, i)->ref;
        for (j = 0; j < e->round_n && e->round_req[j] != r; j++) {}
        if (j == e->round_n && e->round_n < VP_MAX_REQUESTS) e->round_req[e->round_n++] = r;
        e->sigs[i].last_poll_ms = now;
    }
    e->in_round = e->round_n > 0 || every_reqs(e, ev) > 0;
}

static uint32_t checksum(const vp_engine_t *e)
{
    uint32_t sum = e->round_n + (e->in_round ? 256u : 0u);
    uint8_t k;
    for (k = 0; k < e->round_n; k++) sum = sum * 33u + e->round_req[k];
    return sum;
}

uint32_t pp_linear(pp_case_t *c, uint32_t now)
{
    reference_round(&c->e, now);
    return checksum(&c->e);
}

uint32_t pp_mask(pp_case_t *c, uint32_t now)
{
    start_round(&c->e, now);
    return checksum(&c->e);
}

bool pp_verify(pp_case_t *c, uint32_t now)
{
    vp_engine_t reference = c->e;
    unsigned slots = 0;
    reference_round(&reference, now);
    start_round(&c->e, now);
    /* Full state includes deadline changes, first-seen order, stale array
     * tails, empty/every-only rounds, and reset flags, not just a checksum. */
    if (memcmp(&reference, &c->e, sizeof(reference))) return false;
    for (;;) {
        bool a = build_slot(&reference), b = build_slot(&c->e);
        if (a != b || memcmp(&reference, &c->e, sizeof(reference))) return false;
        if (!a) break;
        if (++slots > VP_MAX_REQUESTS + 1) return false;
    }
    return true;
}

void pp_transition(pp_case_t *c, uint32_t seed)
{
    unsigned i;
    c->e.round_no = rng(&seed);
    for (i = 0; i < c->p.n_sig; i++) {
        if (c->sig[i].src_count) c->e.sigs[i].cur = (uint8_t)((rng(&seed) >> 16) & 1);
    }
}
