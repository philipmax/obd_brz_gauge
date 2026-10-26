/*
 * test_engine - vp_engine against a simulated ELM327 (no hardware, no RTOS).
 *
 * The fake adapter answers every command after a short latency, keeps the
 * adapter state the engine changes (ATSH / ATCRA / ATCEA / ATCAF / ATSP /
 * ATH1) and records every command, so the tests can compare the exact
 * traffic. Time is simulated: the loop advances the clock by the wait the
 * engine returns or to the next pending answer.
 *
 * The golden test compares the traffic of every catalog profile with
 * test/host/vehicle/fixtures/golden/<id>.json (missing fixtures are
 * skipped).
 */
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "vp_engine.h"
#include "vp_profile.h"
#include "vp_validate.h"
#include "app_obd_dsp/signal_registry.h"
#include "app_obd_dsp/signal_store.h"
#include "vt.h"
#include "vt_support.h"

#define LOG_MAX  6000
#define CMD_W    24
#define TAB_MAX  32
#define RESP_W   320
#define LATENCY  2

typedef struct {
    char     tx[CMD_W];
    char     hdr[12];
    char     cra[12];
    char     cea[4];
    bool     caf0;
    bool     cfc0;
    uint32_t round;
    uint32_t at_ms;
    uint8_t  phase;
} req_rec_t;

typedef struct {
    char hdr[12];          /* "" = any header */
    char tx[CMD_W];
    char resp[RESP_W];     /* without the trailing "\r\r>" */
    bool silent;           /* no answer at all (timeout) */
} tab_t;

typedef struct {
    uint32_t     now;
    vp_engine_t *eng;
    const vp_profile_t *p;
    bool         verbose;

    char      log[LOG_MAX][CMD_W];
    int       n_log;
    req_rec_t reqs[LOG_MAX];
    int       n_req;
    char      ans[LOG_MAX][CMD_W];
    int       n_ans;

    char     pend[2048];
    size_t   pend_len;
    uint32_t pend_at;
    bool     has_pend;
    char     pend_cmd[CMD_W];

    /* adapter state */
    char hdr[12];
    char cra[12];
    char cea[4];
    bool caf0;
    bool cfc0;
    bool h1;
    bool in_ma;
    int  proto;
    int  redundant;        /* state commands that did not change anything */

    /* responder */
    tab_t       tab[TAB_MAX];
    int         n_tab;
    bool        auto_pos;  /* positive answers built from the profile */
    const char *ma_lines;  /* ATMA output (frames) */
    bool        ma_refuse;
    int         detect_proto;  /* requests fail unless ATSP == this (0 = any) */
    int         timeout_proto; /* selected protocol silently times out the connection probe */
    const char *at_fail;   /* AT command answered "?" */
    bool        at_fail_no_prompt;
    char        stop_once_tx[CMD_W]; /* one transient STOPPED response */
    char        transient_once_tx[CMD_W];
    char        transient_once_resp[CMD_W];
    char       after_fc[2048];
    bool       has_after_fc;

    /* sink side */
    int32_t val[SIG_SLOT_COUNT];
    bool    valid[SIG_SLOT_COUNT];
    int     writes[SIG_SLOT_COUNT];
    int     invals[SIG_SLOT_COUNT];
    int     responses[SIG_SLOT_COUNT];
    uint8_t link;
    uint8_t link_hist[32];
    int     n_link;
    char    veh_keys[8][32];
    int     n_veh;
    int     veh_cleared;
    float   volts;
    int     n_volts;
    bool    elm_up;
    int     proto_detected;
    int     reconnects;
} fake_t;

static fake_t g_f;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Inline profiles are written with ' for " to stay readable. */
static char *q2j(const char *s)
{
    size_t n = strlen(s), i;
    char *o = (char *)malloc(n + 1);
    for (i = 0; i <= n; i++) o[i] = s[i] == '\'' ? '"' : s[i];
    return o;
}

static vp_profile_t *compile_text(const char *json, size_t len)
{
    vp_issue_t items[16];
    vp_issues_t is;
    vp_profile_t *p = NULL;
    vp_compile_err_t r;

    vp_issues_init(&is, items, 16);
    r = vp_compile_json(json, len, vp_validate_default_cfg(), &vt_malloc_allocator, &p, &is);
    if (r != VP_COMPILE_OK) {
        printf("compile failed (%d)\n", (int)r);
        vt_print_issues(&is, "  ");
        return NULL;
    }
    return p;
}

static vp_profile_t *compile_q(const char *q)
{
    char *j = q2j(q);
    vp_profile_t *p = compile_text(j, strlen(j));
    free(j);
    return p;
}

static void fmt_hdr(char *out, size_t n, uint32_t id)
{
    if (id & VP_ID_29BIT) snprintf(out, n, "%08X", (unsigned)(id & VP_ID_MASK));
    else snprintf(out, n, "%03X", (unsigned)id);
}

static int count_cmd(const fake_t *f, const char *cmd)
{
    int i, c = 0;
    for (i = 0; i < f->n_log; i++) c += !strcmp(f->log[i], cmd);
    return c;
}

static int count_ans(const fake_t *f, const char *cmd)
{
    int i, c = 0;
    for (i = 0; i < f->n_ans; i++) c += !strcmp(f->ans[i], cmd);
    return c;
}

static int find_cmd(const fake_t *f, const char *cmd, int from)
{
    int i;
    for (i = from; i < f->n_log; i++) {
        if (!strcmp(f->log[i], cmd)) return i;
    }
    return -1;
}

static void set_resp(fake_t *f, const char *hdr, const char *tx, const char *resp)
{
    int i;
    for (i = 0; i < f->n_tab; i++) {
        if (!strcmp(f->tab[i].tx, tx) && !strcmp(f->tab[i].hdr, hdr)) break;
    }
    if (i == f->n_tab) {
        if (f->n_tab >= TAB_MAX) return;
        f->n_tab++;
    }
    snprintf(f->tab[i].hdr, sizeof(f->tab[i].hdr), "%s", hdr);
    snprintf(f->tab[i].tx, sizeof(f->tab[i].tx), "%s", tx);
    snprintf(f->tab[i].resp, sizeof(f->tab[i].resp), "%s", resp ? resp : "");
    f->tab[i].silent = resp == NULL;
}

static void set_after_fc(fake_t *f, const char *resp)
{
    snprintf(f->after_fc, sizeof(f->after_fc), "%s", resp ? resp : "");
    f->has_after_fc = resp && resp[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* fake adapter                                                        */
/* ------------------------------------------------------------------ */

static void answer(fake_t *f, const char *cmd, const char *text, bool prompt)
{
    size_t n = strlen(text);
    if (!f->has_pend) {
        f->pend_len = 0;
        snprintf(f->pend_cmd, sizeof(f->pend_cmd), "%s", cmd);
    }
    if (f->pend_len + n + 3 >= sizeof(f->pend)) return;
    memcpy(f->pend + f->pend_len, text, n);
    f->pend_len += n;
    if (prompt) {
        memcpy(f->pend + f->pend_len, "\r\r>", 3);
        f->pend_len += 3;
    }
    f->pend_at = f->now + LATENCY;
    f->has_pend = true;
}

static void state_cmd(fake_t *f, char *slot, size_t cap, const char *v)
{
    if (!strcmp(slot, v)) f->redundant++;
    snprintf(slot, cap, "%s", v);
}

static bool auto_answer(fake_t *f, const char *cmd, char *out, size_t cap)
{
    const vp_request_t *rq;
    const vp_source_t *src;
    char tx[CMD_W], hdr[12];
    int r, k, j;
    size_t o = 0;

    if (!f->p) return false;
    rq = vp_profile_requests(f->p);
    src = vp_profile_sources(f->p);
    for (r = 0; r < f->p->n_req; r++) {
        int need = 1;
        for (k = 0; k < rq[r].tx_len; k++) snprintf(tx + 2 * k, sizeof(tx) - 2 * k, "%02X", rq[r].tx[k]);
        tx[2 * rq[r].tx_len] = '\0';
        fmt_hdr(hdr, sizeof(hdr), rq[r].hdr);
        if (strcmp(tx, cmd) || strcmp(hdr, f->hdr)) continue;
        for (k = 0; k < f->p->n_src; k++) {
            if (src[k].kind == VP_SRC_REQ && src[k].ref == r && src[k].len > need) need = src[k].len;
        }
        for (j = 0; j < rq[r].expect_len; j++) o += (size_t)snprintf(out + o, cap - o, "%02X ", rq[r].expect[j]);
        for (j = 0; j < need && o + 4 < cap; j++) o += (size_t)snprintf(out + o, cap - o, "50 ");
        return true;
    }
    return false;
}

static void respond(fake_t *f, const char *cmd)
{
    char buf[RESP_W];
    int i;

    if (cmd[0] == '\0') {
        if (f->in_ma) {
            f->in_ma = false;
            answer(f, cmd, "", true);
        } else {
            answer(f, cmd, "STOPPED", true);
        }
        return;
    }
    if (!strncmp(cmd, "AT", 2)) {
        const char *a = cmd + 2;
        if (f->at_fail && !strcmp(cmd, f->at_fail)) {
            answer(f, cmd, "?", !f->at_fail_no_prompt);
            return;
        }
        if (!strcmp(a, "Z")) {
            f->hdr[0] = f->cra[0] = f->cea[0] = '\0';
            f->caf0 = false;
            f->cfc0 = false;
            f->h1 = false;
            answer(f, cmd, "\r\rELM327 v1.5", true);
            return;
        }
        if (!strcmp(a, "RV")) {
            answer(f, cmd, "12.6V", true);
            return;
        }
        if (!strcmp(a, "MA")) {
            if (f->ma_refuse) {
                answer(f, cmd, "?", true);
                return;
            }
            f->in_ma = true;
            if (f->ma_lines && f->ma_lines[0]) answer(f, cmd, f->ma_lines, false);
            return;
        }
        if (!strcmp(a, "CFC0") || !strcmp(a, "CFC1")) {
            bool v = a[3] == '0';
            if (v == f->cfc0) f->redundant++;
            f->cfc0 = v;
            answer(f, cmd, "OK", true);
            return;
        }
        if (!strncmp(a, "SH", 2)) state_cmd(f, f->hdr, sizeof(f->hdr), a + 2);
        else if (!strncmp(a, "CRA", 3)) state_cmd(f, f->cra, sizeof(f->cra), a + 3);
        else if (!strncmp(a, "CEA", 3)) state_cmd(f, f->cea, sizeof(f->cea), a + 3);
        else if (!strcmp(a, "CAF0") || !strcmp(a, "CAF1")) {
            bool v = a[3] == '0';
            if (v == f->caf0) f->redundant++;
            f->caf0 = v;
        } else if (!strcmp(a, "H1")) f->h1 = true;
        else if (!strcmp(a, "H0")) f->h1 = false;
        else if (!strncmp(a, "SP", 2)) f->proto = (int)strtol(a + 2, NULL, 16);
        else if (!strcmp(a, "AR")) f->cra[0] = '\0';
        answer(f, cmd, "OK", true);
        return;
    }
    /* OBD request */
    if (f->timeout_proto && f->proto == f->timeout_proto && !strcmp(cmd, "0100")) return;
    if (f->detect_proto && f->proto != 0 && f->proto != f->detect_proto) {
        answer(f, cmd, "UNABLE TO CONNECT", true);
        return;
    }
    if (f->stop_once_tx[0] && !strcmp(f->stop_once_tx, cmd)) {
        f->stop_once_tx[0] = '\0';
        answer(f, cmd, "STOPPED", true);
        return;
    }
    if (f->transient_once_tx[0] && !strcmp(f->transient_once_tx, cmd)) {
        f->transient_once_tx[0] = '\0';
        answer(f, cmd, f->transient_once_resp, true);
        return;
    }
    for (i = 0; i < f->n_tab; i++) {
        if (strcmp(f->tab[i].tx, cmd)) continue;
        if (f->tab[i].hdr[0] && strcmp(f->tab[i].hdr, f->hdr)) continue;
        if (f->tab[i].silent) return;
        answer(f, cmd, f->tab[i].resp, true);
        return;
    }
    if (f->auto_pos && auto_answer(f, cmd, buf, sizeof(buf))) {
        answer(f, cmd, buf, true);
        return;
    }
    answer(f, cmd, "NO DATA", true);
}

/* ------------------------------------------------------------------ */
/* sink                                                                */
/* ------------------------------------------------------------------ */

static bool s_send(void *ctx, const char *cmd)
{
    fake_t *f = (fake_t *)ctx;
    if (f->verbose) printf("  %6u > %s\n", (unsigned)f->now, cmd[0] ? cmd : "<CR>");
    if (f->n_log < LOG_MAX) snprintf(f->log[f->n_log++], CMD_W, "%s", cmd);
    if (cmd[0] && strncmp(cmd, "AT", 2) && f->n_req < LOG_MAX) {
        req_rec_t *r = &f->reqs[f->n_req++];
        vp_engine_stats_t st;
        vp_engine_get_stats(f->eng, &st);
        snprintf(r->tx, sizeof(r->tx), "%s", cmd);
        snprintf(r->hdr, sizeof(r->hdr), "%s", f->hdr);
        snprintf(r->cra, sizeof(r->cra), "%s", f->cra);
        snprintf(r->cea, sizeof(r->cea), "%s", f->cea);
        r->caf0 = f->caf0;
        r->cfc0 = f->cfc0;
        r->round = st.rounds;
        r->at_ms = f->now;
        r->phase = st.phase;
    }
    respond(f, cmd);
    return true;
}

static uint32_t s_now(void *ctx) { return ((fake_t *)ctx)->now; }

static void s_set(void *ctx, int slot, int32_t v)
{
    fake_t *f = (fake_t *)ctx;
    if (slot < 0 || slot >= SIG_SLOT_COUNT) return;
    f->val[slot] = v;
    f->valid[slot] = true;
    f->writes[slot]++;
}

static void s_inval(void *ctx, int slot)
{
    fake_t *f = (fake_t *)ctx;
    if (slot < 0 || slot >= SIG_SLOT_COUNT) return;
    f->valid[slot] = false;
    f->invals[slot]++;
}

static void s_speed(void *ctx, uint8_t kmh) { s_set(ctx, SIG_OBD_SPEED, kmh); }

static void s_link(void *ctx, uint8_t st)
{
    fake_t *f = (fake_t *)ctx;
    f->link = st;
    if (f->n_link < 32) f->link_hist[f->n_link++] = st;
}

static int s_veh_reg(void *ctx, const char *key, int32_t mn, int32_t mx, uint32_t stale)
{
    fake_t *f = (fake_t *)ctx;
    (void)mn;
    (void)mx;
    (void)stale;
    if (f->n_veh >= 8) return SIG_SLOT_NONE;
    snprintf(f->veh_keys[f->n_veh], 32, "%s", key);
    return SIG_VEH_BASE + f->n_veh++;
}

static void s_veh_clear(void *ctx) { ((fake_t *)ctx)->veh_cleared++; }

static void s_resp(void *ctx, int slot)
{
    fake_t *f = (fake_t *)ctx;
    if (slot >= 0 && slot < SIG_SLOT_COUNT) f->responses[slot]++;
}

static void s_volt(void *ctx, float v)
{
    fake_t *f = (fake_t *)ctx;
    f->volts = v;
    f->n_volts++;
}

static void s_elm(void *ctx, bool up) { ((fake_t *)ctx)->elm_up = up; }
static void s_proto(void *ctx, uint8_t pr) { ((fake_t *)ctx)->proto_detected = pr; }
static void s_reconnect(void *ctx) { ((fake_t *)ctx)->reconnects++; }

static void s_log(void *ctx, const char *msg)
{
    fake_t *f = (fake_t *)ctx;
    if (f->verbose) printf("  %6u   [%s]\n", (unsigned)f->now, msg);
}

/* ------------------------------------------------------------------ */
/* simulation                                                          */
/* ------------------------------------------------------------------ */

static fake_t *fake_reset(void)
{
    fake_t *f = &g_f;
    memset(f, 0, sizeof(*f));
    f->now = 100000;
    f->verbose = getenv("ENGINE_VERBOSE") != NULL;
    return f;
}

static vp_engine_t *fake_start_with_cfg(fake_t *f, const vp_profile_t *p,
                                        const vp_engine_cfg_t *cfg_in)
{
    vp_engine_sink_t s;
    vp_engine_cfg_t cfg = *cfg_in;

    memset(&s, 0, sizeof(s));
    s.ctx = f;
    s.send = s_send;
    s.now_ms = s_now;
    s.set_signal = s_set;
    s.invalidate = s_inval;
    s.set_speed_kmh = s_speed;
    s.on_link = s_link;
    s.veh_register = s_veh_reg;
    s.veh_clear = s_veh_clear;
    s.on_response = s_resp;
    s.on_voltage = s_volt;
    s.on_elm_link = s_elm;
    s.on_proto_detected = s_proto;
    s.reconnect = s_reconnect;
    s.log = s_log;
    f->p = p;
    f->eng = vp_engine_create(p, &s, &cfg, &vt_malloc_allocator);
    if (f->eng) vp_engine_link_up(f->eng);
    return f->eng;
}

static vp_engine_t *fake_start(fake_t *f, const vp_profile_t *p, uint8_t proto)
{
    vp_engine_cfg_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.proto = proto;
    return fake_start_with_cfg(f, p, &cfg);
}

static void fake_stop(fake_t *f, vp_profile_t *p)
{
    vp_engine_destroy(f->eng);
    f->eng = NULL;
    vp_profile_free(p, &vt_malloc_allocator);
}

static void deliver(fake_t *f)
{
    bool flow_control;

    if (!f->has_pend || (int32_t)(f->pend_at - f->now) > 0) return;
    flow_control = !strcmp(f->pend_cmd, "300800");
    f->has_pend = false;
    if (f->n_ans < LOG_MAX) snprintf(f->ans[f->n_ans++], CMD_W, "%s", f->pend_cmd);
    vp_engine_on_rx(f->eng, (const uint8_t *)f->pend, f->pend_len);
    if (flow_control && f->has_after_fc) {
        char deferred[sizeof(f->after_fc)];
        if (f->verbose) printf("  %6u < deferred FC frames\n", (unsigned)f->now);
        snprintf(deferred, sizeof(deferred), "%s", f->after_fc);
        f->has_after_fc = false;
        answer(f, "", deferred, true);
    }
}

typedef bool (*pred_fn)(fake_t *f, const void *arg);

static bool sim_until(fake_t *f, uint32_t max_ms, pred_fn pred, const void *arg)
{
    uint32_t end = f->now + max_ms;

    while ((int32_t)(end - f->now) > 0) {
        uint32_t w, dt;
        deliver(f);
        w = vp_engine_run(f->eng);
        if (pred && pred(f, arg)) return true;
        dt = w;
        if (f->has_pend) {
            uint32_t until = (int32_t)(f->pend_at - f->now) > 0 ? f->pend_at - f->now : 0;
            if (until < dt) dt = until;
        }
        f->now += dt;
    }
    return pred ? pred(f, arg) : true;
}

static void sim(fake_t *f, uint32_t ms) { (void)sim_until(f, ms, NULL, NULL); }

typedef struct {
    const char *tx;
    int         target;
} ans_arg_t;

static bool pred_ans(fake_t *f, const void *arg)
{
    const ans_arg_t *a = (const ans_arg_t *)arg;
    return count_ans(f, a->tx) >= a->target;
}

/* Run until the next answer to tx has been delivered and processed. */
static bool pump_answer(fake_t *f, const char *tx)
{
    ans_arg_t a;
    a.tx = tx;
    /* an answer already on its way was produced before the caller changed the table */
    a.target = count_ans(f, tx) + 1 + (f->has_pend && !strcmp(f->pend_cmd, tx));
    return sim_until(f, 20000, pred_ans, &a);
}

static bool pred_rounds(fake_t *f, const void *arg)
{
    vp_engine_stats_t st;
    vp_engine_get_stats(f->eng, &st);
    return st.rounds >= *(const uint32_t *)arg;
}

static bool pump_rounds(fake_t *f, uint32_t n)
{
    vp_engine_stats_t st;
    uint32_t target;
    vp_engine_get_stats(f->eng, &st);
    target = st.rounds + n;
    return sim_until(f, 20000, pred_rounds, &target);
}

static void check_log(const fake_t *f, int from, const char *const *exp, int n, const char *what)
{
    int i;
    for (i = 0; i < n; i++) {
        const char *got = from + i < f->n_log ? f->log[from + i] : "<none>";
        VT_CHECK_MSG(!strcmp(got, exp[i]), "%s[%d]: got '%s', want '%s'", what, i, got, exp[i]);
        if (strcmp(got, exp[i])) return;
    }
}

/* ------------------------------------------------------------------ */
/* profiles                                                            */
/* ------------------------------------------------------------------ */

#define P_HEAD(id, bus) "{'defaults':'none','schema':'skygauge.vehicle/1','id':'" id "','rev':1,'name':'T','bus':" bus ","
#define RPN_RPM "[['A',0],['k',256],'*',['A',1],'+',['k',4],'/']"
#define S_RPM_EVERY "{'id':'obd.rpm','rate':'every','src':[{'req':'010C','len':2,'rpn':" RPN_RPM "}]}"
#define S_RPM "{'id':'obd.rpm','src':[{'req':'010C','len':2,'rpn':" RPN_RPM "}]}"
#define S_SPEED "{'id':'obd.speed','src':[{'req':'010D','len':1,'rpn':[['A',0]]}]}"
#define S_CLT "{'id':'obd.coolant_temp','src':[{'req':'0105','len':1,'rpn':[['A',0],['k',40],'-']}]}"
#define DRIVETRAIN "'drivetrain':{'fd':4.1,'tire_m':0.314,'gears':[3.626,2.188,1.541,1.213,1.0,0.767],'tol':0.15},"

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static void test_sparse_defaults(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        "{'schema':'skygauge.vehicle/1','id':'t.sparse','rev':1,'name':'Sparse',"
        "'bus':{'proto':6,'hdr':'7DF','gap_ms':1},'signals':["
        "{'id':'obd.throttle','disabled':true},"
        "{'id':'obd.oil_temp','period_ms':500,'src':[{'req':'221234','hdr':'7E1',"
        "'len':1,'rpn':[['A',0],['k',40],'-']}]},"
        "{'id':'obd.gear','period_ms':200,'src':[{'raw':'18 03 22 DA 2E','hdr':'6F1',"
        "'rx':'618','expect':'62DA2E','len':1,'valid':[0,8],'rpn':[['A',0]]}]}]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->auto_pos = true;
    set_resp(f, "7DF", "010C", "41 0C 1A F8");
    set_resp(f, "7DF", "0105", "41 05 7B");
    set_resp(f, "7DF", "010D", "41 0D 3C");
    set_resp(f, "7E1", "221234", "62 12 34 69");
    set_resp(f, "6F1", "180322DA2E", "F1 04 62 DA 2E 03");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 2));
    VT_CHECK(f->valid[SIG_OBD_RPM]);
    VT_CHECK_INT(f->val[SIG_OBD_RPM], 1726);
    VT_CHECK_INT(f->val[SIG_OBD_SPEED], 60);
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 83);
    VT_CHECK_INT(f->val[SIG_OBD_OIL_TEMP], 65);
    VT_CHECK_INT(f->val[SIG_OBD_GEAR], 3);
    VT_CHECK(count_cmd(f, "221234") > 0);
    VT_CHECK(count_cmd(f, "180322DA2E") > 0);
    VT_CHECK(count_cmd(f, "ATSH7DF") > 1); /* restored after custom headers */
    VT_CHECK_INT(count_cmd(f, "0111"), 0);
    VT_CHECK_INT(count_cmd(f, "015C"), 0);
    VT_CHECK_INT(f->redundant, 0);
    set_resp(f, "7E1", "221234", "NO DATA");
    sim(f, 5000);
    VT_CHECK(!f->valid[SIG_OBD_OIL_TEMP]);
    VT_CHECK_INT(count_cmd(f, "015C"), 0); /* custom failure never adds a hidden fallback */
    fake_stop(f, p);
}

static void test_init_and_lazy_state(void)
{
    static const char *const k_init[] = {
        "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSP6", "ATM0", "ATS0", "ATAT1", "ATAL", "0100",
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.lazy", "{'proto':6,'hdr':'7E0','atst':10,'gap_ms':1}")
        "'requires':{'elm':['ATCEA']},'signals':["
        S_RPM_EVERY "," S_CLT ","
        "{'id':'obd.oil_pressure','src':[{'req':'224436','hdr':'7E1','len':2,'rpn':[['R',0,16,'be'],['k',1000],'/']}]},"
        "{'id':'veh.trans','src':[{'req':'221310','hdr':'7E1','cea':'10','len':2,'rpn':[['A',0]]}]},"
        "{'id':'obd.gear','src':[{'raw':'18 03 22 DA 2E','hdr':'6F1','rx':'618','expect':'62DA2E','len':1,"
        "'valid':[0,8],'rpn':[['A',0]]}]}]}");
    int i;

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 1A F8");
    set_resp(f, "", "0105", "41 05 7B");
    set_resp(f, "7E1", "224436", "62 44 36 0B B8");
    set_resp(f, "7E1", "221310", "62 13 10 2A 00");
    set_resp(f, "6F1", "180322DA2E", "F1 04 62 DA 2E 03");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK_INT(f->n_veh, 1);
    VT_CHECK(!strcmp(f->veh_keys[0], "veh.trans"));
    VT_CHECK_INT(f->link, SIG_LINK_NO_DATA);
    VT_CHECK(f->elm_up);
    VT_CHECK(pump_rounds(f, 1));
    sim(f, 30);
    check_log(f, 0, k_init, (int)(sizeof(k_init) / sizeof(k_init[0])), "init");
    {
        int last_rpm = -1;
        for (i = 0; i < f->n_req; i++) {
            const req_rec_t *r = &f->reqs[i];
            if (r->phase != VP_ENGINE_PH_RUN || r->round != 0) continue;
            if (!strcmp(r->tx, "010C")) last_rpm = i;
            else VT_CHECK_MSG(last_rpm == i - 1, "fast RPM must precede %s", r->tx);
        }
    }
    VT_CHECK_INT(f->val[SIG_OBD_RPM], 1726);
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 83);
    VT_CHECK_INT(f->val[SIG_OBD_OIL_PRESSURE], 300);   /* 3.000 bar, internal 0.01 bar */
    VT_CHECK_INT(f->val[SIG_VEH_BASE], 42);
    VT_CHECK_INT(f->val[SIG_OBD_GEAR], 3);
    VT_CHECK(f->valid[SIG_OBD_GEAR]);
    VT_CHECK_INT(f->link, SIG_LINK_DATA);
    /* longer run: never a redundant state command, every request with its framing */
    sim(f, 5500);
    VT_CHECK(f->n_volts >= 1);
    VT_CHECK_FLT(f->volts, 12.6, 0.01);
    VT_CHECK_INT(f->redundant, 0);
    {
        int first_run = 0;
        while (first_run < f->n_req && f->reqs[first_run].phase != VP_ENGINE_PH_RUN) first_run++;
        VT_CHECK(first_run < f->n_req && !strcmp(f->reqs[first_run].tx, "010C"));
    }
    for (i = 0; i < f->n_req; i++) {
        const req_rec_t *r = &f->reqs[i];
        if (r->phase != VP_ENGINE_PH_RUN) continue;
        if (!strcmp(r->tx, "180322DA2E")) {
            VT_CHECK(r->caf0 && !strcmp(r->cra, "618") && !strcmp(r->hdr, "6F1"));
        } else if (!strcmp(r->tx, "221310")) {
            VT_CHECK(!r->caf0 && !strcmp(r->cea, "10") && !strcmp(r->hdr, "7E1"));
        } else if (!strcmp(r->tx, "224436")) {
            VT_CHECK(!r->caf0 && r->cea[0] == '\0' && !strcmp(r->hdr, "7E1"));
        } else {
            VT_CHECK_MSG(!r->caf0 && r->cra[0] == '\0' && r->cea[0] == '\0' && !strcmp(r->hdr, "7E0"),
                         "request %s framing", r->tx);
        }
    }
    VT_CHECK(count_cmd(f, "ATRV") >= 1);   /* periodic battery reading */
    fake_stop(f, p);
    VT_CHECK_INT(f->veh_cleared, 1);
}

static void test_carscanner_init(void)
{
    static const char *const want[] = {
        "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSP6", "ATM0", "ATS0", "ATAT1", "ATAL",
        "ATSH7E0", "ATFCSH7E0", "ATFCSD300000", "ATFCSM1", "0100"
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.carscanner", "{'proto':6,'hdr':'7E0','init':['ATD','ATD0','ATE0','ATH1','ATSP6','ATM0','ATS0','ATAT1','ATAL',"
        "'ATSH7E0','ATFCSH7E0','ATFCSD300000','ATFCSM1']}")
        "'signals':[" S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 1A F8");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    check_log(f, 0, want, (int)(sizeof(want) / sizeof(want[0])), "carscanner init");
    VT_CHECK_INT(count_cmd(f, "0100"), 1);
    VT_CHECK_INT(count_cmd(f, "ATRV"), 0);
    fake_stop(f, p);
}

static void test_carscanner_adaptive_timing_variant(void)
{
    static const char *const want[] = {
        "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSP6", "ATM0", "ATS0", "ATAT0", "ATAL", "0100"
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.carscanner-atat0", "{'proto':6,'hdr':'7E0','init':['ATZ','ATE0','ATH1','ATSP6','ATM0','ATS0','ATAT0','ATAL']}")
        "'signals':[" S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "0100", "41 00 80 00");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    check_log(f, 0, want, (int)(sizeof(want) / sizeof(want[0])), "ATAT0 init");
    VT_CHECK_INT(count_cmd(f, "ATAT1"), 0);
    fake_stop(f, p);
}

static void test_nissan_consult_init(void)
{
    static const char *const want[] = {
        "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSPB", "ATM0", "ATS0", "ATAT1", "ATAL",
        "2212010401", "0100"
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.nissan-consult", "{'proto':11,'hdr':'7E0','init':['ATZ','ATE0','ATH1','ATSPB','ATM0','ATS0','ATAT1','ATAL',"
        "'2212010401']}") "'signals':[" S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "2212010401", "62 12 01 04 01");
    set_resp(f, "", "0100", "41 00 80 00");
    set_resp(f, "", "010C", "41 0C 1A F8");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    check_log(f, 0, want, (int)(sizeof(want) / sizeof(want[0])), "Nissan Consult II init");
    VT_CHECK_INT(count_cmd(f, "2212010401"), 1);
    fake_stop(f, p);
}

static void test_partial_reset_init(void)
{
    static const char *const want[] = {
        "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSP6", "ATM0", "ATS0", "ATAT1", "ATAL", "0100"
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.partial_init", "{'proto':6,'hdr':'7E0','init':['ATZ']}")
        "'signals':[" S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 1A F8");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    check_log(f, 0, want, (int)(sizeof(want) / sizeof(want[0])), "partial reset init");
    fake_stop(f, p);
}

static void test_partial_init_is_ordered(void)
{
    static const char *const want[] = {
        "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSP6", "ATM0", "ATS0", "ATAT1", "ATAL",
        "ATST19", "ATSH7E0", "0100"
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.partial_order", "{'proto':6,'hdr':'7E0','init':['ATZ','ATST19','ATSP6','ATSH7E0']}")
        "'signals':[" S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "0100", "41 00 80 00");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    check_log(f, 0, want, (int)(sizeof(want) / sizeof(want[0])), "partial init order");
    fake_stop(f, p);
}

static void test_init_wait_tokens(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.initwait", "{'proto':6,'hdr':'7E0','init':['DELAY200','WAIT50']}")
        "'signals':[" S_RPM "]}");
    uint32_t warm_at = 0;
    int i;

    VT_CHECK(p != NULL);
    if (!p) return;
    VT_CHECK(vp_init_wait_ms("delay200", NULL));
    VT_CHECK(vp_init_wait_ms(" WAIT50 ", NULL));
    VT_CHECK(!vp_init_wait_ms("WAIT", NULL));
    VT_CHECK(!vp_init_wait_ms("DELAY65536", NULL));
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    for (i = 0; i < f->n_req; i++) {
        if (!strcmp(f->reqs[i].tx, "0100")) {
            warm_at = f->reqs[i].at_ms;
            break;
        }
    }
    VT_CHECK(warm_at >= 100400); /* explicit 200 + 50 ms waits are not sent */
    VT_CHECK_INT(count_cmd(f, "DELAY200"), 0);
    VT_CHECK_INT(count_cmd(f, "WAIT50"), 0);
    fake_stop(f, p);
}

static void test_bus_connection_commands(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.bus_commands", "{'proto':6,'gap_ms':1,'check_connection':'0902',"
               "'tester_present':'3E00','tester_present_every_ms':200}")
        "'signals':[" S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "0902", "49 02 01 02 03 04");
    set_resp(f, "", "010C", "41 0C 1A F8");
    set_resp(f, "", "3E00", "7E 00");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(count_cmd(f, "0902"), 1);
    VT_CHECK_INT(count_cmd(f, "0100"), 0);
    sim(f, 1000);
    VT_CHECK(count_cmd(f, "3E00") >= 3);
    fake_stop(f, p);
}

static void test_request_command_chain(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.reqchain", "{'proto':6,'hdr':'7E0','gap_ms':1}") "'signals':["
        "{'id':'obd.coolant_temp','src':[{'req':'0105','before':['ATCAF0'],'after':['ATCAF1'],"
        "'len':1,'rpn':[['A',0],['k',40],'-']}]},"
        "{'id':'obd.rpm','src':[{'req':'010C','len':2,'rpn':" RPN_RPM "}]},"
        "{'id':'obd.oil_temp','src':[{'req':'221234','hdr':'7E1','before':['ATCEA10'],"
        "'after':['ATCFC1'],'len':1,'rpn':[['A',0]]}]},"
        "{'id':'obd.speed','src':[{'req':'010D','len':1,'rpn':[['A',0]]}]}]}");
    int first, same_header, deferred, hdr, before, req, flow_after, back_hdr, back_req;

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "7E0", "0105", "41 05 7B");
    set_resp(f, "7E0", "010C", "41 0C 1A F8");
    set_resp(f, "7E1", "221234", "62 12 34 55");
    set_resp(f, "7E0", "010D", "41 0D 3C");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));

    first = find_cmd(f, "ATCAF0", 0);
    same_header = find_cmd(f, "010C", first + 1);
    deferred = find_cmd(f, "ATCAF1", same_header + 1);
    hdr = find_cmd(f, "ATSH7E1", deferred + 1);
    before = find_cmd(f, "ATCEA10", hdr + 1);
    req = find_cmd(f, "221234", before + 1);
    flow_after = find_cmd(f, "ATCFC1", req + 1);
    back_hdr = find_cmd(f, "ATSH7E0", flow_after + 1);
    back_req = find_cmd(f, "010D", back_hdr + 1);
    VT_CHECK(first >= 0 && same_header > first);
    VT_CHECK(deferred > same_header);
    VT_CHECK(hdr > deferred && before > hdr && req > before);
    VT_CHECK(flow_after > req && back_hdr > flow_after && back_req > back_hdr);

    /* A failed BeforeCommand drains the chain and never transmits the OBD request. */
    fake_stop(f, p);
    f = fake_reset();
    p = compile_q(
        P_HEAD("t.reqchain.fail", "{'proto':6,'hdr':'7E0'}") "'signals':["
        "{'id':'obd.coolant_temp','src':[{'req':'0105','before':['ATCAF0'],"
        "'len':1,'rpn':[['A',0],['k',40],'-']}]}]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "7E0", "0105", "41 05 7B");
    f->at_fail = "ATCAF0";
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(count_cmd(f, "ATCAF0"), 1);
    VT_CHECK_INT(count_cmd(f, "0105"), 0);
    fake_stop(f, p);

    /* A later BeforeCommand failure drains the remaining request chain; its
     * AfterCommands belong to a request that was never transmitted. */
    f = fake_reset();
    p = compile_q(
        P_HEAD("t.reqchain.fail-late", "{'proto':6,'hdr':'7E0'}") "'signals':["
        "{'id':'obd.coolant_temp','src':[{'req':'0105','before':['ATCAF0','ATCEA10'],"
        "'after':['ATCAF1'],'len':1,'rpn':[['A',0],['k',40],'-']}]}]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->at_fail = "ATCEA10";
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(count_cmd(f, "ATCAF0"), 1);
    VT_CHECK_INT(count_cmd(f, "ATCEA10"), 1);
    VT_CHECK_INT(count_cmd(f, "0105"), 0);
    VT_CHECK_INT(count_cmd(f, "ATCAF1"), 0);
    fake_stop(f, p);
}

static void test_request_header_explicitness(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.header-explicit", "{'proto':6,'hdr':'7E0','gap_ms':1}") "'signals':["
        "{'id':'obd.rpm','src':[{'req':'010C','after':['ATCAF0'],'len':2,'rpn':[['A',0]]}]},"
        "{'id':'obd.speed','src':[{'req':'010D','hdr':'7E0','len':1,'rpn':[['A',0]]}]}]}");
    int first_req, after, repeated_hdr, second_req;

    VT_CHECK(p != NULL);
    if (!p) return;
    VT_CHECK_INT(p->n_req, 2);
    set_resp(f, "7E0", "010C", "41 0C 0B B8");
    set_resp(f, "7E0", "010D", "41 0D 3C");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));

    first_req = find_cmd(f, "010C", 0);
    after = find_cmd(f, "ATCAF0", first_req + 1);
    repeated_hdr = find_cmd(f, "ATSH7E0", after + 1);
    second_req = find_cmd(f, "010D", repeated_hdr + 1);
    VT_CHECK(first_req >= 0 && after > first_req);
    VT_CHECK(repeated_hdr > after && second_req > repeated_hdr);
    fake_stop(f, p);
}

static void test_stopped_retry(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.stopped", "{'proto':6,'gap_ms':1}") "'signals':[" S_RPM "]}");
    int i, n = 0;
    uint32_t first_ms = 0, retry_ms = 0;

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 1A F8");
    snprintf(f->stop_once_tx, sizeof(f->stop_once_tx), "%s", "010C");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_answer(f, "010C"));
    VT_CHECK(pump_answer(f, "010C"));

    for (i = 0; i < f->n_req; i++) {
        if (f->reqs[i].phase != VP_ENGINE_PH_RUN || strcmp(f->reqs[i].tx, "010C")) continue;
        if (n++ == 0) first_ms = f->reqs[i].at_ms;
        else if (n == 2) retry_ms = f->reqs[i].at_ms;
    }
    VT_CHECK_INT(n, 2);
    VT_CHECK(retry_ms >= first_ms + 200);
    VT_CHECK(f->valid[SIG_OBD_RPM]);
    VT_CHECK_INT(f->val[SIG_OBD_RPM], 1726);
    fake_stop(f, p);
}

static void test_units_and_post(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.units", "{'proto':6}") "'signals':["
        "{'id':'obd.speed','unit':'mph','post':{'zero_le':2},'src':[{'req':'010D','len':1,'rpn':[['A',0]]}]},"
        "{'id':'obd.boost','unit':'bar','src':[{'req':'010B','len':1,'rpn':[['A',0],['k',100],'-',['k',100],'/']}]},"
        "{'id':'obd.battery_voltage','src':[{'req':'0142','len':2,'rpn':[['R',0,16,'be'],['k',1000],'/']}]},"
        "{'id':'obd.afr','unit':'lambda','src':[{'req':'0144','len':2,'rpn':[['R',0,16,'be'],['k',32768],'/']}]}"
        "]}");
    vp_engine_stats_t st;
    int i;

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010D", "41 0D 64");
    set_resp(f, "", "010B", "41 0B C8");
    set_resp(f, "", "0142", "41 42 31 38");
    set_resp(f, "", "0144", "41 44 80 00");
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(f->val[SIG_OBD_SPEED], 161);          /* 100 mph */
    VT_CHECK_INT(f->val[SIG_OBD_BOOST], 100);          /* 1.00 bar */
    VT_CHECK_INT(f->val[SIG_OBD_BATTERY_VOLTAGE], 12600);
    VT_CHECK_INT(f->val[SIG_OBD_AFR], 1470);           /* lambda 1.0 */
    VT_CHECK_INT(f->responses[SIG_OBD_BOOST], 1);
    /* zero_le in the profile unit */
    set_resp(f, "", "010D", "41 0D 02");
    VT_CHECK(pump_answer(f, "010D"));
    VT_CHECK_INT(f->val[SIG_OBD_SPEED], 0);
    /* short answer = sample failure; fail_after (5) failures of the only source -> invalid */
    set_resp(f, "", "010B", "41 0B");
    for (i = 0; i < 4; i++) VT_CHECK(pump_answer(f, "010B"));
    VT_CHECK(f->valid[SIG_OBD_BOOST]);
    VT_CHECK(pump_answer(f, "010B"));
    VT_CHECK(!f->valid[SIG_OBD_BOOST]);
    VT_CHECK_INT(f->invals[SIG_OBD_BOOST], 1);
    /* negative response / NO DATA / garbage are failures too, never matched by strstr */
    set_resp(f, "", "010B", "7F 01 31");
    VT_CHECK(pump_answer(f, "010B"));
    set_resp(f, "", "010B", "SEARCHING...\r41 0B C8");
    VT_CHECK(pump_answer(f, "010B"));
    VT_CHECK(f->valid[SIG_OBD_BOOST]);
    VT_CHECK_INT(f->val[SIG_OBD_BOOST], 100);
    set_resp(f, "", "010B", "BUS INIT: ...OK\r41 0B C9");
    VT_CHECK(pump_answer(f, "010B"));
    VT_CHECK_INT(f->val[SIG_OBD_BOOST], 101);
    /* a short answer still matches the prefix: only the 7F counts as unmatched */
    vp_engine_get_stats(f->eng, &st);
    VT_CHECK(st.rx_fail >= 1);
    VT_CHECK(st.rx_ok >= 10);
    fake_stop(f, p);
}

static void test_hold(void)
{
    static const struct { const char *resp; int want; } k_seq[] = {
        { "41 05 5A", 50 }, { "41 05 6E", 50 }, { "41 05 6E", 50 }, { "41 05 6E", 70 },
        { "41 05 70", 72 }, { "41 05 5A", 72 },
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.hold", "{'proto':6}") "'signals':["
        "{'id':'obd.coolant_temp','src':[{'req':'0105','len':1,'hold':{'delta':8,'max_hold':2,'gap_ms':3000},"
        "'rpn':[['A',0],['k',40],'-']}]}]}");
    size_t i;

    VT_CHECK(p != NULL);
    if (!p) return;
    fake_start(f, p, 0);
    for (i = 0; i < sizeof(k_seq) / sizeof(k_seq[0]); i++) {
        set_resp(f, "", "0105", k_seq[i].resp);
        VT_CHECK(pump_answer(f, "0105"));
        VT_CHECK_MSG(f->val[SIG_OBD_COOLANT_TEMP] == k_seq[i].want, "hold step %u: %d != %d",
                     (unsigned)i, (int)f->val[SIG_OBD_COOLANT_TEMP], k_seq[i].want);
    }
    /* a gap longer than gap_ms resets the hold: the jump is accepted at once */
    set_resp(f, "", "0105", NULL);
    sim(f, 3500);
    set_resp(f, "", "0105", "41 05 96");
    sim(f, 3100);   /* the silent request times out after 3 s */
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 110);
    fake_stop(f, p);
}

static void test_temp_hyst(void)
{
    static const struct { const char *resp; int want; int writes; } k_seq[] = {
        { "41 5C 82", 90, 1 },  /* first sample */
        { "41 5C 84", 91, 2 },  /* small step: 0.65 / 0.35 EMA */
        { "41 5C 91", 91, 2 },  /* 105: medium step, pending 1 */
        { "41 5C 91", 91, 2 },
        { "41 5C 91", 91, 2 },
        { "41 5C 91", 98, 3 },  /* 4th confirmation: half way */
        { "41 5C F0", 98, 3 },  /* 200 C: out of the plausible range */
        { "41 5C 28", 98, 3 },  /* 0 C: large step, pending */
        { "41 5C 28", 98, 3 },
        { "41 5C 28", 0, 4 },   /* 3rd confirmation: jump */
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.hyst", "{'proto':6}") "'signals':["
        "{'id':'obd.oil_temp','filter':'temp_hyst','src':[{'req':'015C','len':1,'rpn':[['A',0],['k',40],'-']}]}]}");
    size_t i;

    VT_CHECK(p != NULL);
    if (!p) return;
    fake_start(f, p, 0);
    for (i = 0; i < sizeof(k_seq) / sizeof(k_seq[0]); i++) {
        set_resp(f, "", "015C", k_seq[i].resp);
        VT_CHECK(pump_answer(f, "015C"));
        VT_CHECK_MSG(f->val[SIG_OBD_OIL_TEMP] == k_seq[i].want && f->writes[SIG_OBD_OIL_TEMP] == k_seq[i].writes,
                     "hyst step %u: %d (%d writes), want %d (%d)", (unsigned)i,
                     (int)f->val[SIG_OBD_OIL_TEMP], f->writes[SIG_OBD_OIL_TEMP], k_seq[i].want,
                     k_seq[i].writes);
    }
    fake_stop(f, p);
}

static void test_fallback_chain(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.chain", "{'proto':6}") "'signals':["
        "{'id':'obd.coolant_temp','src':["
        "{'req':'221234','len':1,'fail_after':2,'rpn':[['A',0],['k',40],'-']},"
        "{'req':'0105','len':1,'rpn':[['A',0],['k',40],'-']}]}]}");
    int first;

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "0105", "41 05 5A");
    fake_start(f, p, 0);
    VT_CHECK(pump_answer(f, "0105"));
    first = find_cmd(f, "0105", 0);
    VT_CHECK_INT(count_cmd(f, "221234"), 2);
    VT_CHECK(first > find_cmd(f, "221234", 0));
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 50);
    /* the fallback fails too: after its 5 failures the chain wraps -> invalid, back to src 0 */
    set_resp(f, "", "0105", "NO DATA");
    sim(f, 400);
    VT_CHECK(!f->valid[SIG_OBD_COOLANT_TEMP]);
    VT_CHECK(f->invals[SIG_OBD_COOLANT_TEMP] >= 1);
    VT_CHECK(count_cmd(f, "221234") >= 3);
    fake_stop(f, p);
}

static void test_gear(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.gear", "{'proto':6,'gap_ms':1}") DRIVETRAIN "'signals':["
        S_RPM "," S_SPEED ","
        "{'id':'obd.gear','fallback':'ratio','src':[{'req':'221234','len':1,'fail_after':1,'valid':[0,8],"
        "'rpn':[['A',0]]}]}]}");
    float sp3;

    VT_CHECK(p != NULL);
    if (!p) return;
    /* pure function: total = rpm * 0.377 * tire / speed vs gears[i] * fd */
    sp3 = 3000.0f * 0.377f * 0.314f / (1.541f * 4.1f);
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 3000.0f, sp3, 0), 3);
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 3000.0f, 3000.0f * 0.377f * 0.314f / (3.626f * 4.1f), 0), 1);
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 2000.0f, 2000.0f * 0.377f * 0.314f / (0.767f * 4.1f), 0), 6);
    /* 3rd/4th bands overlap around total 5.5: the current gear is kept */
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 3000.0f, 3000.0f * 0.377f * 0.314f / 5.5f, 3), 3);
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 3000.0f, 3000.0f * 0.377f * 0.314f / 5.5f, 4), 4);
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 3000.0f, 3000.0f * 0.377f * 0.314f / 5.5f, 0), 3);
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 0.0f, 50.0f, 3), 0);
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 1500.0f, 2.0f, 3), 0);        /* revving, stopped */
    VT_CHECK_INT(vp_engine_gear_from_ratio(p, 3000.0f, 200.0f, 2), 2);      /* clutch slip: keep */

    /* engine: the gear DID fails (fail_after 1) -> ratio fallback */
    set_resp(f, "", "010C", "41 0C 2E E0");   /* 3000 rpm */
    set_resp(f, "", "010D", "41 0D 38");      /* 56 km/h */
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 2));
    VT_CHECK(f->valid[SIG_OBD_GEAR]);
    VT_CHECK_INT(f->val[SIG_OBD_GEAR], 3);
    /* the DID answers: its value wins */
    set_resp(f, "", "221234", "62 12 34 05");
    VT_CHECK(pump_rounds(f, 2));
    VT_CHECK_INT(f->val[SIG_OBD_GEAR], 5);
    /* DID gone again, rpm lost -> gear invalid */
    set_resp(f, "", "221234", "NO DATA");
    set_resp(f, "", "010C", "NO DATA");
    VT_CHECK(pump_rounds(f, 6));
    VT_CHECK(!f->valid[SIG_OBD_RPM]);
    VT_CHECK(!f->valid[SIG_OBD_GEAR]);
    fake_stop(f, p);

    /* derive: ratio only */
    f = fake_reset();
    p = compile_q(P_HEAD("t.gear2", "{'proto':6}") DRIVETRAIN "'signals':["
                  S_RPM "," S_SPEED ",{'id':'obd.gear','derive':'ratio'}]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 2E E0");
    set_resp(f, "", "010D", "41 0D 38");
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(f->val[SIG_OBD_GEAR], 3);
    set_resp(f, "", "010D", "41 0D 00");
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(f->val[SIG_OBD_GEAR], 0);
    VT_CHECK_INT(count_cmd(f, "221234"), 0);
    fake_stop(f, p);
}

static void test_link_states(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(P_HEAD("t.link", "{'proto':6}") "'signals':[" S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 0B B8");
    fake_start(f, p, 0);
    VT_CHECK_INT(f->n_link, 1);
    VT_CHECK_INT(f->link_hist[0], SIG_LINK_NO_DATA);
    VT_CHECK(pump_answer(f, "010C"));
    VT_CHECK_INT(f->link, SIG_LINK_DATA);
    VT_CHECK_INT(f->val[SIG_OBD_RPM], 750);
    set_resp(f, "", "010C", "NO DATA");
    sim(f, 2500);
    VT_CHECK_INT(f->link, SIG_LINK_DATA);
    sim(f, 1000);
    VT_CHECK_INT(f->link, SIG_LINK_NO_DATA);
    vp_engine_link_down(f->eng);
    VT_CHECK_INT(f->link, SIG_LINK_NONE);
    VT_CHECK(!f->elm_up);
    VT_CHECK(!f->valid[SIG_OBD_RPM]);
    VT_CHECK_INT(f->n_link, 4);
    VT_CHECK(f->n_link == 4 && f->link_hist[1] == SIG_LINK_DATA && f->link_hist[2] == SIG_LINK_NO_DATA);
    /* down: no traffic */
    {
        int n = f->n_log;
        sim(f, 1000);
        VT_CHECK_INT(f->n_log, n);
    }
    /* up again: bring-up starts over */
    {
        int atz_before = count_cmd(f, "ATZ");
    vp_engine_link_up(f->eng);
    sim(f, 400);
    VT_CHECK_INT(count_cmd(f, "ATZ"), atz_before);
    sim(f, 200);
    VT_CHECK_INT(count_cmd(f, "ATZ"), atz_before + 1);
    }
    VT_CHECK_INT(f->link, SIG_LINK_NO_DATA);
    fake_stop(f, p);
}

static void test_elm_adapter_check(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(P_HEAD("t.elm_check", "{'proto':6}") "'signals':[" S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;

    /* The adapter check is delayed and must happen before any ECU request. */
    set_resp(f, "", "0100", "NO DATA");
    set_resp(f, "", "010C", "41 0C 0B B8");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK_INT(f->n_log, 0);
    sim(f, 400);
    VT_CHECK_INT(count_cmd(f, "ATZ"), 0);
    sim(f, 200);
    VT_CHECK_INT(count_cmd(f, "ATZ"), 1);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(count_cmd(f, "0100"), 1);
    VT_CHECK_INT(f->reconnects, 0);
    VT_CHECK_INT(f->val[SIG_OBD_RPM], 750);
    fake_stop(f, p);

    /* A missing prompt fails the adapter check and requests reconnect. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.elm_check_fail", "{'proto':6}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->at_fail = "ATZ";
    f->at_fail_no_prompt = true;
    VT_CHECK(fake_start(f, p, 0) != NULL);
    sim(f, 3200);
    VT_CHECK_INT(count_cmd(f, "ATZ"), 1);
    VT_CHECK_INT(count_cmd(f, "0100"), 0);
    VT_CHECK_INT(f->reconnects, 1);
    {
        vp_engine_stats_t st;
        vp_engine_get_stats(f->eng, &st);
        VT_CHECK_INT(st.phase, VP_ENGINE_PH_RECONNECT);
    }
    fake_stop(f, p);
}

static void test_heal_and_reconnect(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(P_HEAD("t.heal", "{'proto':6}") "'signals':[" S_RPM "]}");
    vp_engine_stats_t st;

    VT_CHECK(p != NULL);
    if (!p) return;
    fake_start(f, p, 0);
    sim(f, 11500);
    vp_engine_get_stats(f->eng, &st);
    VT_CHECK_INT(st.heals, 1);
    VT_CHECK_INT(count_cmd(f, "ATZ"), 2);
    VT_CHECK_INT(f->reconnects, 0);
    sim(f, 25000);
    vp_engine_get_stats(f->eng, &st);
    VT_CHECK_INT(f->reconnects, 1);
    VT_CHECK_INT(st.phase, VP_ENGINE_PH_RECONNECT);
    VT_CHECK_INT(count_cmd(f, "ATZ"), 3);
    fake_stop(f, p);
}

#define P_MON(extra_src) \
    P_HEAD("t.mon", "{'proto':6,'gap_ms':1}") \
    "'requires':{'elm':['ATMA']},'monitor':{'mode':'probe','every_ms':500,'window_ms':120,'stale_ms':15000}," \
    "'signals':[" S_RPM_EVERY "," \
    "{'id':'obd.coolant_temp','src':[{'can':'360','bit':24,'bits':8,'rpn':[['X'],['k',40],'-']}" extra_src "]}," \
    "{'id':'obd.oil_temp','src':[{'can':'360','bit':16,'bits':8,'rpn':[['X'],['k',40],'-']}]}]}"

static void test_monitor(void)
{
    static const char *const k_mon[] = { "ATH1", "ATMA", "", "ATAR", "ATH0" };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(P_MON(",{'req':'0105','len':1,'rpn':[['A',0],['k',40],'-']}"));
    vp_engine_stats_t st;
    int at, n0105;

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 0B B8");
    set_resp(f, "", "0105", "41 05 64");
    f->ma_lines = "360 00 00 82 78 00 00 00 00\r140 00 00 00 00 00 00 33 00\r"
                  "1:360 00 00 82 78 00 00 00 00\rCAN ERROR\r";
    fake_start(f, p, 0);
    sim(f, 2000);   /* first window 500 ms after the bring-up (~750 ms) */
    at = find_cmd(f, "ATMA", 0);
    VT_CHECK(at > 0);
    if (at > 0) check_log(f, at - 1, k_mon, 5, "monitor");
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 80);
    VT_CHECK_INT(f->val[SIG_OBD_OIL_TEMP], 90);
    VT_CHECK_INT(count_cmd(f, "0105"), 0);
    vp_engine_get_stats(f->eng, &st);
    VT_CHECK(st.mon_windows >= 2);
    VT_CHECK(st.can_frames >= 3);
    VT_CHECK(f->valid[SIG_OBD_RPM]);
    VT_CHECK_INT(f->redundant, 0);

    /* frames stop: 5 empty windows -> coolant falls back to 0105, oil (CAN only) invalid */
    f->ma_lines = "140 00 00 00 00 00 00 33 00\r";
    sim(f, 3500);
    VT_CHECK(count_cmd(f, "0105") > 0);
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 60);
    VT_CHECK(!f->valid[SIG_OBD_OIL_TEMP]);
    /* frames back: the CAN source is promoted again, 0105 polling stops */
    f->ma_lines = "360 00 00 82 78 00 00 00 00\r";
    sim(f, 1200);
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 80);
    VT_CHECK(f->valid[SIG_OBD_OIL_TEMP]);
    n0105 = count_cmd(f, "0105");
    sim(f, 1500);
    VT_CHECK_INT(count_cmd(f, "0105"), n0105);
    fake_stop(f, p);

    /* adapter without ATMA: monitor off for the link, CAN sources skipped */
    f = fake_reset();
    p = compile_q(P_MON(",{'req':'0105','len':1,'rpn':[['A',0],['k',40],'-']}"));
    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 0B B8");
    set_resp(f, "", "0105", "41 05 64");
    f->ma_refuse = true;
    fake_start(f, p, 0);
    sim(f, 3000);
    vp_engine_get_stats(f->eng, &st);
    VT_CHECK(!st.mon_ok);
    VT_CHECK_INT(count_cmd(f, "ATMA"), 1);
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 60);
    VT_CHECK(!f->valid[SIG_OBD_OIL_TEMP]);
    fake_stop(f, p);
}

static void test_can_line_formats(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.can29", "{'proto':7,'hdr':'18DB33F1','gap_ms':1}")
        "'requires':{'elm':['ATMA']},'monitor':{'mode':'probe','every_ms':300,'window_ms':100,'stale_ms':15000},"
        "'signals':["
        "{'id':'obd.coolant_temp','src':[{'can':'18FEEE00','bit':0,'bits':8,'rpn':[['X'],['k',40],'-']}]},"
        "{'id':'obd.oil_temp','src':[{'can':'5A0','bit':8,'bits':8,'rpn':[['X'],['k',40],'-']}]}]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    /* 29-bit as one token, 29-bit as four tokens, 11-bit with a DLC digit */
    f->ma_lines = "18FEEE00 78 00 00 00 00 00 00 00\r18 FE EE 00 79 00\r5A0 8 00 82 00 00 00 00 00 00\r";
    fake_start(f, p, 0);
    sim(f, 1500);
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 81);
    VT_CHECK_INT(f->val[SIG_OBD_OIL_TEMP], 90);
    /* This profile has only monitor/CAN sources, so no request needs the
     * default transmit header during the run. */
    VT_CHECK(find_cmd(f, "ATSP7", 0) >= 0);
    fake_stop(f, p);
}

static void test_multiframe(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.multi", "{'proto':6}") "'signals':["
        "{'id':'obd.oil_temp','src':[{'req':'2101','multiframe':true,'byte_from_end':5,'rpn':[['X'],['k',40],'-']}]},"
        "{'id':'veh.first','src':[{'req':'2101','multiframe':true,'len':3,'rpn':[['A',2]]}]},"
        S_RPM "]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    /* ELM numbered frames (CAF1, H0) with the byte count line */
    set_resp(f, "", "2101",
             "014\r0: 61 01 11 22 33 44\r1: 55 66 77 88 99 AA BB\r2: CC DD 82 EE FF 00 00");
    set_resp(f, "", "010C", "41 0C 0B B8");
    fake_start(f, p, 0);
    VT_CHECK(pump_answer(f, "2101"));
    VT_CHECK_INT(f->val[SIG_OBD_OIL_TEMP], 90);
    VT_CHECK_INT(f->val[SIG_VEH_BASE], 0x33);
    /* raw ISO-TP frames (first frame + consecutive frames) */
    set_resp(f, "", "2101", "10 14 61 01 11 22 33 44");
    set_after_fc(f, "30 02 00\r21 55 66 77 88 99 AA BB\r22 CC DD 78 EE FF 00 00");
    VT_CHECK(pump_answer(f, "2101"));
    sim(f, 50); /* allow request throttling plus the FC/continuation latency. */
    VT_CHECK_INT(f->val[SIG_OBD_OIL_TEMP], 80);
    VT_CHECK(count_cmd(f, "ATCAF0") >= 1);
    VT_CHECK(count_cmd(f, "ATCFC0") >= 1);
    {
        int req = find_cmd(f, "2101", 0);
        int fc = find_cmd(f, "300800", req + 1);
        VT_CHECK(req >= 0 && fc > req);
    }
    /* The next ordinary request restores automatic framing and flow control. */
    VT_CHECK(pump_answer(f, "010C"));
    {
        int req = find_cmd(f, "2101", 0);
        int caf1 = find_cmd(f, "ATCAF1", req + 1);
        int cfc1 = find_cmd(f, "ATCFC1", req + 1);
        VT_CHECK(req >= 0 && caf1 > req && cfc1 > req);
    }
    /* compact hex single frame (spaces off) */
    set_resp(f, "", "2101", "7E8 6101AABBCCDDEE016E02030405");
    VT_CHECK(pump_answer(f, "2101"));
    VT_CHECK_INT(f->val[SIG_OBD_OIL_TEMP], 70);
    VT_CHECK_INT(f->val[SIG_VEH_BASE], 0xCC);
    fake_stop(f, p);
}

static void test_rates(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.rates", "{'proto':6,'gap_ms':1}") "'signals':["
        S_RPM_EVERY "," S_CLT ","
        "{'id':'veh.slow','rate':'slow','div':4,'src':[{'req':'221310','len':1,'rpn':[['A',0]]}]},"
        "{'id':'veh.period','rate':'on_demand','period_ms':500,'src':[{'req':'0101','len':4,'rpn':[['A',0]]}]}]}");
    vp_engine_stats_t st0, st1;
    int n_clt, n_slow, n_per, n_rpm;

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 0B B8");
    set_resp(f, "", "0105", "41 05 64");
    set_resp(f, "", "221310", "62 13 10 07");
    set_resp(f, "", "0101", "41 01 00 07 E5 00");
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 1));
    vp_engine_get_stats(f->eng, &st0);
    n_clt = count_cmd(f, "0105");
    n_slow = count_cmd(f, "221310");
    n_per = count_cmd(f, "0101");
    n_rpm = count_cmd(f, "010C");
    sim(f, 2000);
    vp_engine_get_stats(f->eng, &st1);
    {
        int rounds = (int)(st1.rounds - st0.rounds);
        int clt = count_cmd(f, "0105") - n_clt;
        int slow = count_cmd(f, "221310") - n_slow;
        int per = count_cmd(f, "0101") - n_per;
        int rpm = count_cmd(f, "010C") - n_rpm;
        VT_CHECK(rounds > 20);
        VT_CHECK_MSG(clt >= rounds - 1 && clt <= rounds + 1, "clt %d rounds %d", clt, rounds);
        VT_CHECK_MSG(slow >= rounds / 4 - 1 && slow <= rounds / 4 + 1, "slow %d rounds %d", slow, rounds);
        VT_CHECK_MSG(per >= 3 && per <= 5, "period %d", per);
        VT_CHECK_MSG(rpm >= clt + slow + per - 1, "every %d vs slots %d", rpm, clt + slow + per);
    }
    VT_CHECK_INT(f->val[SIG_VEH_BASE], 7);
    VT_CHECK_INT(f->val[SIG_VEH_BASE + 1], 0);
    fake_stop(f, p);
}

static void test_screen_demand_polling(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.screen-demand", "{'proto':6,'gap_ms':1}")
        "'drivetrain':{'fd':4.1,'tire_m':0.314,'gears':[3.626,2.188,1.541,1.213,1.0,0.767],'tol':0.15},"
        "'signals':[" S_RPM "," S_SPEED "," S_CLT ","
        "{'id':'obd.gear','derive':'ratio'},"
        "{'id':'veh.keep','src':[{'req':'221310','len':1,'rpn':[['A',0]]}]}]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 0B B8");
    set_resp(f, "", "010D", "41 0D 32");
    set_resp(f, "", "0105", "41 05 64");
    set_resp(f, "", "221310", "62 13 10 07");
    fake_start(f, p, 0);
    vp_engine_set_obd_demand_mask(f->eng, (uint16_t)(1u << SIG_OBD_GEAR));
    VT_CHECK(pump_rounds(f, 1));
    sim(f, 300);
    VT_CHECK(count_cmd(f, "010C") > 0);  /* RPM exception */
    VT_CHECK(count_cmd(f, "010D") > 0);  /* ratio-derived gear dependency */
    VT_CHECK(count_cmd(f, "0105") == 0); /* not displayed */
    VT_CHECK(count_cmd(f, "221310") > 0); /* retained for multi-gauge slaves */

    vp_engine_set_obd_demand_mask(f->eng, (uint16_t)(1u << SIG_OBD_SPEED));
    sim(f, 300);
    VT_CHECK(count_cmd(f, "010D") > 0);
    VT_CHECK(count_cmd(f, "0105") == 0);
    fake_stop(f, p);
}

static void test_functional_fallback(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.functional-fallback", "{'proto':6,'hdr':'7E0','gap_ms':1}")
        "'signals':[" S_RPM "]}");
    int physical, functional;

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "7E0", "010C", "7F 01 11");
    set_resp(f, "7DF", "010C", "41 0C 0B B8");
    VT_CHECK(fake_start(f, p, 0) != NULL);
    VT_CHECK(pump_rounds(f, 1));
    physical = -1;
    functional = -1;
    for (int i = 0; i < f->n_req; i++) {
        if (!strcmp(f->reqs[i].tx, "010C") && !strcmp(f->reqs[i].hdr, "7E0")) physical = i;
        if (!strcmp(f->reqs[i].tx, "010C") && !strcmp(f->reqs[i].hdr, "7DF")) functional = i;
    }
    VT_CHECK(physical >= 0 && functional > physical);
    VT_CHECK_INT(f->val[SIG_OBD_RPM], 750);
    {
        int functional_count = 0;
        int physical_count = 0;
        for (int i = 0; i < f->n_req; i++) {
            if (!strcmp(f->reqs[i].tx, "010C") && !strcmp(f->reqs[i].hdr, "7DF")) functional_count++;
            if (!strcmp(f->reqs[i].tx, "010C") && !strcmp(f->reqs[i].hdr, "7E0")) physical_count++;
        }
        VT_CHECK_INT(physical_count, 1);
        sim(f, 500);
        functional_count = 0;
        physical_count = 0;
        for (int i = 0; i < f->n_req; i++) {
            if (!strcmp(f->reqs[i].tx, "010C") && !strcmp(f->reqs[i].hdr, "7DF")) functional_count++;
            if (!strcmp(f->reqs[i].tx, "010C") && !strcmp(f->reqs[i].hdr, "7E0")) physical_count++;
        }
        VT_CHECK(functional_count > 1); /* fallback stays on 7DF without another physical probe */
        VT_CHECK_INT(physical_count, 1);
    }
    fake_stop(f, p);
}

static void test_detect(void)
{
    static const uint8_t custom_protocols[] = { 9, 6 };
    static const uint8_t timeout_protocols[] = { 3, 6 };
    static const uint8_t invalid_protocols[] = { 0, 9 };
    static const char *const k_det[] = {
        "ATZ", "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSP6", "ATM0", "ATS0", "ATAT1", "ATAL", "0100",
        "ATD", "ATD0", "ATE0", "ATH1", "ATSP6", "ATM0", "ATS0", "ATAT1", "ATAL", "0100",
    };
    static const char *const k_up[] = {
        "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSP6", "ATM0", "ATS0", "ATAT1", "ATAL", "0100",
    };
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(P_HEAD("t.det", "{'proto':0}") "'signals':[" S_RPM "]}");
    vp_engine_stats_t st;
    int mark;

    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 6;
    set_resp(f, "", "0100", "41 00 80 00");
    set_resp(f, "", "010C", "41 0C 0B B8");
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 1));
    check_log(f, 0, k_det, (int)(sizeof(k_det) / sizeof(k_det[0])), "detect");
    VT_CHECK_INT(f->proto_detected, 6);
    vp_engine_get_stats(f->eng, &st);
    VT_CHECK_INT(st.proto, 6);
    VT_CHECK_INT(f->val[SIG_OBD_RPM], 750);
    /* reconnect: the detected protocol is reused */
    vp_engine_link_down(f->eng);
    mark = f->n_log;
    vp_engine_link_up(f->eng);
    VT_CHECK(pump_rounds(f, 1));
    check_log(f, mark, k_up, (int)(sizeof(k_up) / sizeof(k_up[0])), "re-init");
    fake_stop(f, p);

    /* A timed-out K-line probe must be interrupted and drained before the
     * next protocol selection is sent. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det-drain", "{'proto':0}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 6;
    f->timeout_proto = 3;
    set_resp(f, "", "0100", "41 00 80 00");
    set_resp(f, "", "010C", "41 0C 0B B8");
    {
        vp_engine_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.detect_protocols = timeout_protocols;
        cfg.detect_protocol_count = (uint8_t)(sizeof(timeout_protocols) / sizeof(timeout_protocols[0]));
        cfg.detect_timeout_ms = 30;
        fake_start_with_cfg(f, p, &cfg);
    }
    VT_CHECK(pump_rounds(f, 1));
    {
        int drain = -1, next_atz = -1, next_atsp6 = -1;
        for (int i = 0; i < f->n_log; i++) {
            if (drain < 0 && f->log[i][0] == '\0') drain = i;
            if (drain >= 0 && next_atz < 0 && !strcmp(f->log[i], "ATZ")) next_atz = i;
            if (drain >= 0 && next_atsp6 < 0 && !strcmp(f->log[i], "ATSP6")) next_atsp6 = i;
        }
        VT_CHECK(drain >= 0 && next_atz > drain && next_atsp6 > next_atz);
    }
    VT_CHECK_INT(f->proto_detected, 6);
    fake_stop(f, p);

    /* A generic profile without obd.rpm must still use its configured
     * connection check for protocol detection. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det-no-rpm", "{'proto':0,'check_connection':'0902'}")
                  "'signals':[" S_SPEED "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 6;
    set_resp(f, "", "0902", "49 02 57 49 4E 56 49 4E");
    set_resp(f, "", "010D", "41 0D 3C");
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(f->proto_detected, 6);
    VT_CHECK(count_cmd(f, "010C") == 0);
    VT_CHECK(count_cmd(f, "0902") >= 2); /* probe plus warm check */
    VT_CHECK(f->valid[SIG_OBD_SPEED]);
    fake_stop(f, p);

    /* A later candidate must be reached when the earlier CAN protocols fail. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det-fallback", "{'proto':0}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 9;
    set_resp(f, "", "0100", "41 00 80 00");
    set_resp(f, "", "010C", "41 0C 0B B8");
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(count_cmd(f, "ATSP6"), 3);
    VT_CHECK_INT(count_cmd(f, "ATSP7"), 3);
    VT_CHECK_INT(count_cmd(f, "ATSP4"), 3);
    VT_CHECK_INT(count_cmd(f, "ATSP5"), 3);
    VT_CHECK_INT(count_cmd(f, "ATSP3"), 3);
    VT_CHECK_INT(count_cmd(f, "ATSP1"), 3);
    VT_CHECK_INT(count_cmd(f, "ATSP2"), 3);
    VT_CHECK_INT(count_cmd(f, "ATSP8"), 3);
    VT_CHECK_INT(count_cmd(f, "ATSP9"), 2); /* successful probe + final init */
    VT_CHECK_INT(f->proto_detected, 9);
    fake_stop(f, p);

    /* A transient BUS INIT error retries the same candidate. It must not
     * consume one of the candidate attempts or jump to the next protocol. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det-bus-init-retry", "{'proto':0}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 6;
    set_resp(f, "", "0100", "41 00 80 00");
    set_resp(f, "", "010C", "41 0C 0B B8");
    snprintf(f->transient_once_tx, sizeof(f->transient_once_tx), "%s", "0100");
    snprintf(f->transient_once_resp, sizeof(f->transient_once_resp), "%s", "BUS INIT: ERROR");
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(count_cmd(f, "ATSP6"), 2);
    VT_CHECK_INT(count_cmd(f, "0100"), 3);
    VT_CHECK_INT(f->proto_detected, 6);
    fake_stop(f, p);

    /* A caller can override the probe order for a known adapter/vehicle. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det-custom", "{'proto':0}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 9;
    set_resp(f, "", "0100", "41 00 80 00");
    {
        vp_engine_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.detect_protocols = custom_protocols;
        cfg.detect_protocol_count = (uint8_t)(sizeof(custom_protocols) / sizeof(custom_protocols[0]));
        fake_start_with_cfg(f, p, &cfg);
    }
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(count_cmd(f, "ATSP9"), 2); /* probe plus final init */
    VT_CHECK_INT(count_cmd(f, "ATSP6"), 0);
    fake_stop(f, p);

    /* Invalid custom candidates fail closed to the engine defaults. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det-invalid-custom", "{'proto':0}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 9;
    set_resp(f, "", "0100", "41 00 80 00");
    {
        vp_engine_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.detect_protocols = invalid_protocols;
        cfg.detect_protocol_count = (uint8_t)(sizeof(invalid_protocols) / sizeof(invalid_protocols[0]));
        fake_start_with_cfg(f, p, &cfg);
    }
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK(count_cmd(f, "ATSP6") >= 1);
    VT_CHECK_INT(f->proto_detected, 9);
    fake_stop(f, p);

    /* nothing answers: keep the adapter in ATSP0 auto mode without persisting a protocol */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det2", "{'proto':0}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 99;
    fake_start(f, p, 0);
    sim(f, 30000);
    VT_CHECK(count_cmd(f, "ATSP0") >= 1);
    VT_CHECK_INT(f->proto_detected, 0);
    fake_stop(f, p);

    /* a configured protocol skips detection */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det3", "{'proto':0}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    fake_start(f, p, 8);
    sim(f, 50);
    VT_CHECK_INT(count_cmd(f, "ATSP1"), 0);
    sim(f, 1000);
    VT_CHECK(find_cmd(f, "ATSP8", 0) > 0);
    fake_stop(f, p);

    /* Auto-detection must also hide K-line response headers on a protocol
     * 3 candidate, before its positive 0100 response is parsed. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.det-kline", "{'proto':0}") "'signals':[" S_RPM "]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    f->detect_proto = 3;
    set_resp(f, "", "0100", "41 00 80 00");
    set_resp(f, "", "010C", "41 0C 0B B8");
    fake_start(f, p, 0);
    sim(f, 20000);
    int atsp3 = find_cmd(f, "ATSP3", 0);
    int ath0 = find_cmd(f, "ATH0", atsp3 + 1);
    VT_CHECK(atsp3 >= 0 && ath0 > atsp3);
    VT_CHECK_INT(f->proto_detected, 3);
    VT_CHECK_INT(count_cmd(f, "ATSH7E0"), 0);
    fake_stop(f, p);

    /* Imported ISO K-line profiles can carry explicit CAN headers, but the
     * ELM must keep native addressing and hide K-line response headers. */
    f = fake_reset();
    p = compile_q(P_HEAD("t.iso-kline", "{'proto':3,'hdr':'7E0','init':['ATH1']}")
        "'signals':[{'id':'obd.rpm','src':[{'req':'010C','hdr':'7E0','len':2,"
        "'rpn':[['A',0],['k',256],'*',['A',1],'+',['k',4],'/']}]}]}");
    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "010C", "41 0C 0B B8");
    fake_start(f, p, 0);
    VT_CHECK(pump_rounds(f, 1));
    VT_CHECK_INT(count_cmd(f, "ATSH7E0"), 0);
    VT_CHECK_INT(count_cmd(f, "ATH1"), 0);
    VT_CHECK(count_cmd(f, "ATH0") > 0);
    VT_CHECK(f->valid[SIG_OBD_RPM]);
    fake_stop(f, p);
}

static void test_at_refused_and_state_fail(void)
{
    fake_t *f = fake_reset();
    vp_profile_t *p = compile_q(
        P_HEAD("t.atfail", "{'proto':6}") "'signals':[" S_CLT ","
        "{'id':'obd.oil_pressure','src':[{'req':'224436','hdr':'7E1','len':2,'rpn':[['R',0,16,'be'],['k',1000],'/']}]}]}");

    VT_CHECK(p != NULL);
    if (!p) return;
    set_resp(f, "", "0105", "41 05 64");
    set_resp(f, "7E1", "224436", "62 44 36 0B B8");
    f->at_fail = "ATSH7E1";
    fake_start(f, p, 0);
    sim(f, 1500);
    /* the request is never sent with the wrong header */
    VT_CHECK_INT(count_cmd(f, "224436"), 0);
    VT_CHECK(!f->valid[SIG_OBD_OIL_PRESSURE]);
    VT_CHECK_INT(f->val[SIG_OBD_COOLANT_TEMP], 60);
    f->at_fail = NULL;
    sim(f, 500);
    VT_CHECK(count_cmd(f, "224436") > 0);
    VT_CHECK_INT(f->val[SIG_OBD_OIL_PRESSURE], 300);
    fake_stop(f, p);
}

/* ------------------------------------------------------------------ */
/* golden                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char hdr[12];
    char tx[CMD_W];
    char rx[12];
    bool raw;
} gq_t;

static int gq_add(gq_t *list, int n, const gq_t *q)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!strcmp(list[i].hdr, q->hdr) && !strcmp(list[i].tx, q->tx)) return n;
    }
    list[n] = *q;
    return n + 1;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void golden_one(const char *cat_path, const char *golden_dir, int *n_done, int *n_skip)
{
    char gpath[512], want[16];
    char *text, *gtext;
    size_t len, glen;
    vp_profile_t *p;
    cJSON *g;
    const cJSON *reqs, *it;
    gq_t round[VP_MAX_REQUESTS], fast[VP_MAX_REQUESTS], expect[2 * VP_MAX_REQUESTS * 2];
    int n_round = 0, n_fast = 0, n_exp = 0, i, k, before;
    fake_t *f;

    text = vt_read_file(cat_path, &len);
    VT_CHECK_MSG(text != NULL, "read %s", cat_path);
    if (!text) return;
    p = compile_text(text, len);
    free(text);
    VT_CHECK_MSG(p != NULL, "compile %s", cat_path);
    if (!p) return;
    snprintf(gpath, sizeof(gpath), "%s/%s.json", golden_dir, p->id);
    gtext = vt_read_file(gpath, &glen);
    if (!gtext) {
        printf("  SKIP %s (no golden fixture)\n", p->id);
        (*n_skip)++;
        vp_profile_free(p, &vt_malloc_allocator);
        return;
    }
    g = cJSON_ParseWithLength(gtext, glen);
    free(gtext);
    VT_CHECK_MSG(g != NULL, "parse %s", gpath);
    if (!g) {
        vp_profile_free(p, &vt_malloc_allocator);
        return;
    }
    before = vt_failures;
    f = fake_reset();
    f->auto_pos = true;
    fake_start(f, p, 6);
    VT_CHECK(pump_rounds(f, 2));

    /* bring-up: the runtime owns the common Car Scanner sequence.  The
     * historical fixture remains authoritative for request projections, not
     * for the retired legacy ATAL/ATST/warm-up sequence. */
    snprintf(want, sizeof(want), "ATSP%X", p->proto ? p->proto : 6u);
    {
        static const char *const base[] = {
            "ATZ", "ATD", "ATD0", "ATE0", "ATH1", NULL, "ATM0", "ATS0", "ATAT1"
        };
        for (i = 0; i < (int)(sizeof(base) / sizeof(base[0])); i++) {
            const char *expected = base[i] ? base[i] : want;
            VT_CHECK_MSG(i < f->n_log && !strcmp(f->log[i], expected),
                         "%s init[%d]: got '%s', want '%s'", p->id, i,
                         i < f->n_log ? f->log[i] : "", expected);
        }
    }
    VT_CHECK_INT(count_cmd(f, "0100"), 1);

    /* first round: every round request once (signal order), fast ones after each slot */
    reqs = cJSON_GetObjectItemCaseSensitive(g, "requests");
    cJSON_ArrayForEach(it, reqs) {
        gq_t q;
        const cJSON *src = cJSON_GetObjectItemCaseSensitive(it, "src");
        memset(&q, 0, sizeof(q));
        if (!cJSON_IsNumber(src) || src->valueint != 0) continue;
        snprintf(q.hdr, sizeof(q.hdr), "%s", jstr(it, "hdr"));
        snprintf(q.tx, sizeof(q.tx), "%s", jstr(it, "tx"));
        snprintf(q.rx, sizeof(q.rx), "%s", jstr(it, "rx"));
        q.raw = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "raw")) ||
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "multi"));
        if (!strcmp(jstr(it, "class"), "fast")) n_fast = gq_add(fast, n_fast, &q);
        else n_round = gq_add(round, n_round, &q);
    }
    for (i = 0; i < n_round; i++) {
        for (k = 0; k < n_fast; k++) {
            if (strcmp(fast[k].hdr, round[i].hdr) || strcmp(fast[k].tx, round[i].tx)) expect[n_exp++] = fast[k];
        }
        for (k = 0; k < n_fast; k++) {
            if (!strcmp(fast[k].hdr, round[i].hdr) && !strcmp(fast[k].tx, round[i].tx)) break;
        }
        if (k == n_fast) expect[n_exp++] = round[i];
    }
    if (n_round == 0) {
        for (k = 0; k < n_fast; k++) expect[n_exp++] = fast[k];
    }
    k = 0;
    for (i = 0; i < f->n_req; i++) {
        const req_rec_t *r = &f->reqs[i];
        if (r->phase != VP_ENGINE_PH_RUN || r->round != 0) continue;
        if (k < n_exp) {
            const gq_t *e = &expect[k];
            VT_CHECK_MSG(!strcmp(r->tx, e->tx) && !strcmp(r->hdr, e->hdr), "%s round[%d]: got %s@%s, want %s@%s",
                         p->id, k, r->tx, r->hdr, e->tx, e->hdr);
            VT_CHECK_MSG(r->caf0 == e->raw, "%s %s: CAF0 %d", p->id, r->tx, (int)r->caf0);
            VT_CHECK_MSG(!strcmp(r->cra, e->rx), "%s %s: ATCRA '%s', want '%s'", p->id, r->tx, r->cra, e->rx);
        }
        k++;
    }
    VT_CHECK_MSG(k == n_exp, "%s: %d requests in round 0, want %d", p->id, k, n_exp);
    VT_CHECK_MSG(f->redundant == 0, "%s: %d redundant state commands", p->id, f->redundant);

    /* CAN monitor windows only for profiles with a monitor */
    sim(f, 1500);
    if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(g, "monitor"))) {
        VT_CHECK_MSG(count_cmd(f, "ATMA") >= 1, "%s: no monitor window", p->id);
    } else {
        VT_CHECK_MSG(count_cmd(f, "ATMA") == 0, "%s: unexpected ATMA", p->id);
    }
    printf("  %s %s\n", vt_failures == before ? "ok  " : "FAIL", p->id);
    (*n_done)++;
    cJSON_Delete(g);
    fake_stop(f, p);
}

static void test_golden(void)
{
    DIR *d = opendir(ET_CATALOG_DIR);
    struct dirent *de;
    int n_done = 0, n_skip = 0;

    if (!d) {
        printf("  SKIP golden: no catalog at %s\n", ET_CATALOG_DIR);
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char path[512];
        size_t n = strlen(de->d_name);
        if (n < 13 || strcmp(de->d_name + n - 13, ".vehicle.json")) continue;
        snprintf(path, sizeof(path), "%s/%s", ET_CATALOG_DIR, de->d_name);
        golden_one(path, ET_GOLDEN_DIR, &n_done, &n_skip);
    }
    closedir(d);
    printf("  golden: %d compared, %d skipped\n", n_done, n_skip);
}

/* ENGINE_ONLY=<name> runs one test (with ENGINE_VERBOSE=1 for the traffic). */
#define RUN(fn)                                                               \
    do {                                                                      \
        const char *only_ = getenv("ENGINE_ONLY");                            \
        if (!only_ || !strcmp(only_, #fn)) VT_RUN(fn);                        \
    } while (0)

int main(void)
{
    RUN(test_sparse_defaults);
    RUN(test_init_and_lazy_state);
    RUN(test_request_command_chain);
    RUN(test_request_header_explicitness);
    RUN(test_stopped_retry);
    RUN(test_carscanner_init);
    RUN(test_carscanner_adaptive_timing_variant);
    RUN(test_nissan_consult_init);
    RUN(test_partial_reset_init);
    RUN(test_partial_init_is_ordered);
    RUN(test_init_wait_tokens);
    RUN(test_bus_connection_commands);
    RUN(test_units_and_post);
    RUN(test_hold);
    RUN(test_temp_hyst);
    RUN(test_fallback_chain);
    RUN(test_gear);
    RUN(test_link_states);
    RUN(test_elm_adapter_check);
    RUN(test_heal_and_reconnect);
    RUN(test_monitor);
    RUN(test_can_line_formats);
    RUN(test_multiframe);
    RUN(test_rates);
    RUN(test_screen_demand_polling);
    RUN(test_functional_fallback);
    RUN(test_detect);
    RUN(test_at_refused_and_state_fail);
    RUN(test_golden);
    return VT_SUMMARY("engine");
}
