/* Include actual private queue functions. engine_core's second engine object
 * is not extracted from the archive, since this TU defines its public API.
 * QC_ENGINE_SOURCE is only for temporary, host-only baselines/negative controls. */
#include "queue_copy_probe.h"
#ifndef QC_ENGINE_SOURCE
#define QC_ENGINE_SOURCE "../../../main/vehicle/vp_engine.c"
#endif
#include QC_ENGINE_SOURCE

struct qc_log { unsigned n; char msg[96]; };
struct qc_case {
    vp_engine_t e;
    vp_engine_t old;
    struct qc_log logs[2];
    unsigned active;
};

static uint32_t rng(uint32_t *s)
{
    *s = *s * UINT32_C(1664525) + UINT32_C(1013904223);
    return *s;
}
static void log_step(void *ctx, const char *msg)
{
    qc_case_t *c = ctx;
    struct qc_log *l = &c->logs[c->active];
    l->n++;
    memset(l->msg, 0, sizeof(l->msg));
    snprintf(l->msg, sizeof(l->msg), "%s", msg);
}
qc_case_t *qc_create(unsigned head, unsigned count, uint32_t seed)
{
    qc_case_t *c;
    unsigned char *bytes;
    size_t i;
    if (head >= ENG_STEPS || count > ENG_STEPS) return NULL;
    c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    /* Queue methods must leave unrelated engine bytes and dirty slots alone. */
    bytes = (unsigned char *)&c->e;
    for (i = 0; i < sizeof(c->e); i++) bytes[i] = (unsigned char)(rng(&seed) >> 24);
    memset(&c->e.sink, 0, sizeof(c->e.sink));
    c->e.sink.ctx = c;
    c->e.sink.log = log_step;
    c->e.q_head = (uint8_t)head;
    c->e.q_n = (uint8_t)count;
    memcpy(&c->old, &c->e, sizeof(c->e));
    return c;
}
void qc_destroy(qc_case_t *c) { free(c); }
unsigned qc_count(const qc_case_t *c) { return c->e.q_n; }

/* Independent, pre-change reference (including drop logging). */
static eng_step_t *old_push(vp_engine_t *e, uint8_t kind, const char *cmd, uint16_t delay)
{
    eng_step_t *s;
    if (e->q_n >= ENG_STEPS) {
        elog(e, "step queue full, dropped %s", cmd);
        return NULL;
    }
    s = &e->q[(e->q_head + e->q_n) % ENG_STEPS];
    memset(s, 0, sizeof(*s));
    snprintf(s->cmd, sizeof(s->cmd), "%s", cmd);
    s->kind = kind;
    s->delay_ms = delay;
    s->req = -1;
    e->q_n++;
    return s;
}
static void old_pop(vp_engine_t *e, eng_step_t *out)
{
    *out = e->q[e->q_head];
    e->q_head = (uint8_t)((e->q_head + 1) % ENG_STEPS);
    e->q_n--;
}
bool qc_push(qc_case_t *c, uint8_t kind, const char *cmd, uint16_t delay)
{
    eng_step_t *a, *b;
    ptrdiff_t ai, bi;
    memset(c->logs, 0, sizeof(c->logs));
    c->active = 0; a = q_push(&c->e, kind, cmd, delay);
    c->active = 1; b = old_push(&c->old, kind, cmd, delay);
    ai = a ? a - c->e.q : -1;
    bi = b ? b - c->old.q : -1;
    return ai == bi && !memcmp(&c->e, &c->old, sizeof(c->e)) &&
        !memcmp(&c->logs[0], &c->logs[1], sizeof(c->logs[0]));
}
bool qc_pop(qc_case_t *c)
{
    eng_step_t a, b;
    if (!c->e.q_n || c->e.q_n != c->old.q_n) return false;
    memset(&a, 0xA5, sizeof(a)); memset(&b, 0xA5, sizeof(b));
    q_pop(&c->e, &a); old_pop(&c->old, &b);
    return !memcmp(&a, &b, sizeof(a)) && !memcmp(&c->e, &c->old, sizeof(c->e));
}
static uint32_t checksum(const eng_step_t *s, uint32_t tick)
{
    uint32_t sum = tick ^ s->kind ^ s->delay_ms ^ (uint16_t)s->req ^ s->val;
    size_t i;
    for (i = 0; i < sizeof(s->cmd); i++) sum = sum * 33u + (uint8_t)s->cmd[i];
    return sum;
}
uint32_t qc_format(qc_case_t *c, const char *cmd, uint32_t tick)
{
    eng_step_t s;
    if (!old_push(&c->old, (uint8_t)(tick & 7u), cmd, (uint16_t)tick)) abort();
    old_pop(&c->old, &s);
    return checksum(&s, tick);
}
uint32_t qc_copy(qc_case_t *c, const char *cmd, uint32_t tick)
{
    eng_step_t s;
    if (!q_push(&c->e, (uint8_t)(tick & 7u), cmd, (uint16_t)tick)) abort();
    q_pop(&c->e, &s);
    return checksum(&s, tick);
}

/* Pre-change state construction; use the same production queue copy for both
 * timing paths so the previous queue optimization is not counted twice. */
static void old_fmt_id(char *out, size_t cap, const char *prefix, uint32_t id)
{
    if (id & VP_ID_29BIT) {
        snprintf(out, cap, "%s%08X", prefix, (unsigned)(id & VP_ID_MASK));
    } else {
        snprintf(out, cap, "%s%03X", prefix, (unsigned)(id & 0x7FFu));
    }
}
static void old_state(vp_engine_t *e, uint8_t st, uint32_t val, int16_t req)
{
    char cmd[ENG_CMD_CAP];
    eng_step_t *s;
    switch (st) {
    case ST_HDR:
        old_fmt_id(cmd, sizeof(cmd), "ATSH", val);
        break;
    case ST_RX:
        if (val == 0) snprintf(cmd, sizeof(cmd), "ATCRA");
        else old_fmt_id(cmd, sizeof(cmd), "ATCRA", val);
        break;
    case ST_CEA:
        if (val == ENG_CEA_OFF_VAL) snprintf(cmd, sizeof(cmd), "ATCEA");
        else snprintf(cmd, sizeof(cmd), "ATCEA%02X", (unsigned)(val & 0xFFu));
        break;
    default:
        snprintf(cmd, sizeof(cmd), "ATCAF%u", val ? 0u : 1u);
        break;
    }
    s = q_push(e, K_STATE, cmd, 0);
    if (!s) return;
    s->st = st;
    s->val = val;
    s->req = req;
}
bool qc_state(qc_case_t *c, uint8_t st, uint32_t val, int16_t req)
{
    memset(c->logs, 0, sizeof(c->logs));
    c->active = 0; push_state(&c->e, st, val, req);
    c->active = 1; old_state(&c->old, st, val, req);
    return !memcmp(&c->e, &c->old, sizeof(c->e)) &&
        !memcmp(&c->logs[0], &c->logs[1], sizeof(c->logs[0]));
}
/* Timing uses the same engine address in both wrappers. Correctness above
 * still compares independent old/new state. Whole-source baseline timing is
 * also required: compiler context changes reference-only constant paths. */
uint32_t qc_state_format(qc_case_t *c, uint8_t st, uint32_t val, int16_t req)
{
    eng_step_t s;
    if (c->e.q_n >= ENG_STEPS) abort();
    old_state(&c->e, st, val, req);
    q_pop(&c->e, &s);
    return checksum(&s, val) ^ s.st;
}
uint32_t qc_state_copy(qc_case_t *c, uint8_t st, uint32_t val, int16_t req)
{
    eng_step_t s;
    if (c->e.q_n >= ENG_STEPS) abort();
    push_state(&c->e, st, val, req);
    q_pop(&c->e, &s);
    return checksum(&s, val) ^ s.st;
}
