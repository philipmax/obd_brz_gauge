/*
 * vp_engine - compiled vehicle profile -> ELM327 traffic -> signal values.
 * See vp_engine.h for the behaviour summary; the decode pipeline follows
 * contracts/vehicle-rpn.md section 3 step by step (comments "P1".."P10").
 *
 * Execution model: one command in flight at a time. A command is a step
 * {cmd, kind, delay}; the response is collected until the '>' prompt (or a
 * timeout), handled, then the step's delay runs before the next command.
 * Bring-up, detection and monitor windows queue their steps up front; the
 * polling loop plans one step at a time so adapter state (ATSH / ATCRA /
 * ATCEA / ATCAF) is only changed right before the request that needs it.
 */

#include "vp_engine.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vp_common.h"
#include "vp_validate.h"
#include "app_obd_dsp/signal_registry.h"
#include "app_obd_dsp/signal_store.h"

#define ENG_RX_CAP          512
#define ENG_SCRATCH_CAP     256     /* parsed response bytes / reassembled payload */
#define ENG_MAX_LINES       32
#define ENG_STEPS           24
#define ENG_CMD_CAP         20
#if ENG_CMD_CAP < 14
#error "state commands require prefix + 8 hex digits + NUL"
#endif

#define ENG_DEF_TIMEOUT     3000
#define ENG_DEF_DETECT      2000
#define ENG_DEF_LINK_HOLD   3000
#define ENG_DEF_VBAT        5000
#define ENG_DEF_HEAL        10000
#define ENG_HEAL_RECONNECT  3       /* heals in a row before asking for a reconnect */
#define ENG_AT_DELAY        30
#define ENG_DETECT_SETTLE   200
#define ENG_DETECT_SP_DELAY 50
#define ENG_DETECT_FALLBACK 6
#define ENG_DETECT_LAST     12
#define ENG_WARM_COUNT      3
#define ENG_WARM_DELAY      150
#define ENG_MA_STOP_MS      300
#define ENG_CONT_WINDOW_MS  5000
#define ENG_IDLE_MS         50
#define ENG_WAIT_CAP        100
#define ENG_DEF_GEAR_TOL    0.15f
#define ENG_EMA_ALPHA       0.3f
#define ENG_HYST_NONE       (-100)

#define ENG_ID_UNKNOWN      0xFFFFFFFFu   /* hdr / rx not known (never a valid id) */
#define ENG_INDEX_NONE      0xFFu

#if VP_MAX_REQUESTS > 32
#error "poll request mask requires VP_MAX_REQUESTS <= 32"
#endif
#define ENG_CEA_OFF         (-1)
#define ENG_ST_UNKNOWN      (-2)
#define ENG_CEA_OFF_VAL     0x100u        /* step value for "ATCEA" (off) */

enum { K_AT = 0, K_INIT, K_STATE, K_DETECT, K_WARM, K_RV, K_REQ, K_MA };
enum { ST_HDR = 0, ST_RX, ST_CEA, ST_CAF };
enum { DONE_PROMPT = 0, DONE_TIMEOUT, DONE_FAIL };

typedef struct {
    char     cmd[ENG_CMD_CAP];
    uint8_t  kind;          /* K_* */
    uint8_t  st;            /* ST_* for K_STATE */
    uint16_t delay_ms;      /* pause after the response */
    uint16_t timeout_ms;    /* 0 = cfg.resp_timeout_ms */
    int16_t  req;           /* request index (K_REQ, K_STATE before a request), else -1 */
    uint32_t val;           /* state value (K_STATE), protocol (K_DETECT) */
} eng_step_t;

typedef struct {
    /* fixed at create */
    double   factor;        /* P8: RPN result unit -> internal */
    int16_t  slot;          /* signal_store slot, SIG_SLOT_NONE = not driven */
    /* runtime */
    uint8_t  cur;           /* current source (relative to src_first) */
    uint8_t  fails;         /* consecutive failures of the current source */
    bool     exhausted;
    bool     fallback_on;   /* ratio fallback active (obd.gear) */
    bool     can_seen;      /* a frame matched during the current monitor window */
    bool     can_stale;     /* invalidated by monitor.stale_ms */
    bool     filt_init;
    uint8_t  hyst_cnt;
    int16_t  hyst_pending;
    float    filt;
    uint32_t last_poll_ms;
    uint32_t last_can_ms;
} eng_sig_t;

typedef struct {
    float    last;
    uint32_t last_ms;
    uint8_t  held;
    bool     has;
} eng_hold_t;

typedef struct {
    uint16_t off;
    uint8_t  n;
    bool     num;
} eng_line_t;

struct vp_engine {
    const vp_profile_t  *p;
    const vp_request_t  *req;
    const vp_signal_t   *sig;
    const vp_source_t   *src;
    const vp_can_rule_t *can;
    vp_engine_sink_t     sink;
    vp_engine_cfg_t      cfg;
    vp_allocator_t       alloc;

    uint8_t  phase;
    uint8_t  link;
    uint8_t  run_proto;     /* protocol used for ATSP */
    uint8_t  det_proto;     /* detected this session (0 = none) */
    uint8_t  detect_next;
    bool     mon_ok;
    bool     busy;
    bool     prompt;

    /* in-flight step */
    eng_step_t cur;
    uint32_t   sent_ms;
    bool       ma_stopping;
    bool       ma_refused;
    uint32_t   ma_end_ms;

    /* queued steps (ring) */
    eng_step_t q[ENG_STEPS];
    uint8_t    q_head;
    uint8_t    q_n;
    uint32_t   next_ms;

    /* what the adapter is believed to be set to */
    uint32_t hdr;
    uint32_t rx;            /* 0 = no filter */
    uint32_t base_rx;       /* filter set by bus.init, restored after rx requests */
    int16_t  cea;           /* ENG_CEA_OFF, 0..255, ENG_ST_UNKNOWN */
    int8_t   caf0;          /* 0 = CAF1, 1 = CAF0, ENG_ST_UNKNOWN */

    /* polling rounds */
    uint8_t  round_req[VP_MAX_REQUESTS];
    uint8_t  round_n;
    uint8_t  round_pos;
    bool     in_round;
    bool     every_done;
    uint8_t  slot_req[1 + VP_MAX_EVERY_SIGNALS];
    uint8_t  slot_n;
    uint8_t  slot_pos;
    uint8_t  every_sig[VP_MAX_EVERY_SIGNALS];
    uint8_t  n_every;
    bool     req_sent;
    uint32_t round_no;
    uint32_t last_req_ms;

    /* Static request -> source index. A response only walks sources that can
     * reference that request, while preserving profile signal/source order. */
    uint8_t  req_head[VP_MAX_REQUESTS];
    uint8_t  req_tail[VP_MAX_REQUESTS];
    uint8_t  req_next[VP_MAX_SOURCES];
    uint8_t  poll_sig[VP_MAX_SIGNALS];
    uint8_t  n_poll;
    uint64_t factor_identity_mask;

    /* timers */
    uint32_t last_data_ms;  /* last decoded write (sys.obd_link 2) */
    uint32_t last_good_ms;  /* last good sample (self-heal) */
    uint32_t last_vbat_ms;
    uint32_t last_mon_ms;
    uint8_t  heal_count;

    /* derived signals */
    int16_t  sig_gear;
    int32_t  rpm;
    int32_t  speed;
    int8_t   gear;
    bool     rpm_valid;
    bool     speed_valid;
    bool     gear_valid;

    vp_engine_stats_t st;

    eng_sig_t  sigs[VP_MAX_SIGNALS];
    eng_hold_t holds[VP_MAX_SOURCES];
    uint16_t   rx_len;
    char       rxbuf[ENG_RX_CAP + 1];
    uint8_t    lines[ENG_SCRATCH_CAP];
    uint8_t    payload[ENG_SCRATCH_CAP];
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__)
static void elog(vp_engine_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static void elog(vp_engine_t *e, const char *fmt, ...)
{
    char b[96];
    va_list ap;

    if (!e->sink.log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    e->sink.log(e->sink.ctx, b);
}

static uint32_t cap_wait(uint32_t ms)
{
    if (ms < 1) return 1;
    return ms > ENG_WAIT_CAP ? ENG_WAIT_CAP : ms;
}

static bool elapsed(uint32_t now, uint32_t since, uint32_t ms)
{
    return (uint32_t)(now - since) >= ms;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* State commands have fixed prefixes and at most eight hex digits. */
static const char eng_hex[] = "0123456789ABCDEF";

static void fmt_hex(char *out, uint32_t value, unsigned digits)
{
    out[digits] = '\0';
    while (digits) {
        out[--digits] = eng_hex[value & 0x0Fu];
        value >>= 4;
    }
}

static void fmt_id(char *out, uint32_t id)
{
    if (id & VP_ID_29BIT) fmt_hex(out, id & VP_ID_MASK, 8);
    else fmt_hex(out, id & 0x7FFu, 3);
}

static uint32_t resp_timeout(const vp_engine_t *e)
{
    return e->cfg.resp_timeout_ms ? e->cfg.resp_timeout_ms : ENG_DEF_TIMEOUT;
}

static uint32_t link_hold(const vp_engine_t *e)
{
    return e->cfg.link_hold_ms ? e->cfg.link_hold_ms : ENG_DEF_LINK_HOLD;
}

static uint32_t vbat_every(const vp_engine_t *e)
{
    return e->cfg.vbat_every_ms ? e->cfg.vbat_every_ms : ENG_DEF_VBAT;
}

static uint32_t heal_after(const vp_engine_t *e)
{
    return e->cfg.heal_ms ? e->cfg.heal_ms : ENG_DEF_HEAL;
}

static bool mon_usable(const vp_engine_t *e)
{
    return e->p->mon_mode != VP_MON_NONE && e->p->n_can > 0 && e->mon_ok;
}

static const vp_source_t *cur_src(const vp_engine_t *e, int i)
{
    const vp_signal_t *s = &e->sig[i];
    if (s->src_count == 0) return NULL;
    return &e->src[s->src_first + e->sigs[i].cur];
}

/* ------------------------------------------------------------------ */
/* sink wrappers                                                       */
/* ------------------------------------------------------------------ */

static void set_link(vp_engine_t *e, uint8_t state)
{
    e->link = state;
    e->sink.on_link(e->sink.ctx, state);
}

static void put(vp_engine_t *e, int slot, int32_t v, uint32_t now)
{
    if (e->link != SIG_LINK_DATA) set_link(e, SIG_LINK_DATA);
    e->last_data_ms = now;
    if (slot == SIG_OBD_SPEED) {
        int32_t k = v < 0 ? 0 : (v > 255 ? 255 : v);
        e->sink.set_speed_kmh(e->sink.ctx, (uint8_t)k);
    } else {
        e->sink.set_signal(e->sink.ctx, slot, v);
    }
}

/* ------------------------------------------------------------------ */
/* step queue                                                          */
/* ------------------------------------------------------------------ */

static void q_clear(vp_engine_t *e)
{
    e->q_head = 0;
    e->q_n = 0;
}

static eng_step_t *q_push(vp_engine_t *e, uint8_t kind, const char *cmd, uint16_t delay_ms)
{
    eng_step_t *s;

    if (e->q_n >= ENG_STEPS) {
        elog(e, "step queue full, dropped %s", cmd);
        return NULL;
    }
    s = &e->q[(e->q_head + e->q_n) % ENG_STEPS];
    memset(s, 0, sizeof(*s));
    /* The zeroed tail supplies the terminator, even for truncated commands. */
    for (size_t i = 0; i < sizeof(s->cmd) - 1 && cmd[i]; i++) {
        s->cmd[i] = cmd[i];
    }
    s->kind = kind;
    s->delay_ms = delay_ms;
    s->req = -1;
    e->q_n++;
    return s;
}

static void q_pop(vp_engine_t *e, eng_step_t *out)
{
    *out = e->q[e->q_head];
    e->q_head = (uint8_t)((e->q_head + 1) % ENG_STEPS);
    e->q_n--;
}

static void push_state(vp_engine_t *e, uint8_t st, uint32_t val, int16_t req)
{
    char c[ENG_CMD_CAP];
    eng_step_t *s;

    /* Fixed byte stores avoid out-of-line memcpy under IDF compiler flags.
     * Keep constant-only snprintf calls: the compiler folds them to stores. */
    switch (st) {
    case ST_HDR:
        c[0] = 'A'; c[1] = 'T';
        c[2] = 'S'; c[3] = 'H';
        fmt_id(c + 4, val);
        break;
    case ST_RX:
        if (val == 0) snprintf(c, sizeof(c), "ATCRA");
        else {
            c[0] = 'A'; c[1] = 'T';
            c[2] = 'C'; c[3] = 'R'; c[4] = 'A';
            fmt_id(c + 5, val);
        }
        break;
    case ST_CEA:
        if (val == ENG_CEA_OFF_VAL) snprintf(c, sizeof(c), "ATCEA");
        else {
            c[0] = 'A'; c[1] = 'T';
            c[2] = 'C'; c[3] = 'E'; c[4] = 'A';
            fmt_hex(c + 5, val & 0xFFu, 2);
        }
        break;
    default:
        c[0] = 'A'; c[1] = 'T';
        c[2] = 'C'; c[3] = 'A'; c[4] = 'F';
        c[5] = val ? '0' : '1'; c[6] = '\0';
        break;
    }
    s = q_push(e, K_STATE, c, 0);
    if (!s) return;
    s->st = st;
    s->val = val;
    s->req = req;
}

/* ------------------------------------------------------------------ */
/* response parsing                                                    */
/* ------------------------------------------------------------------ */

/*
 * One response line as bytes: 2-digit tokens (or compact even-length hex),
 * optional "N:" line number. Returns 1 with *n bytes, 0 when the line is not
 * a byte line (text such as NO DATA, a 3-digit byte count), -1 when the
 * scratch buffer is full.
 */
static int parse_byte_line(const char *s, size_t len, uint8_t *out, size_t cap, size_t *n,
                           bool *numbered)
{
    size_t i = 0, cnt = 0, j;

    *numbered = false;
    *n = 0;
    while (i < len && s[i] == ' ') i++;
    j = i;
    while (j < len && j - i < 3 && hexval(s[j]) >= 0) j++;
    if (j < len && s[j] == ':' && j > i && j - i <= 2) {
        *numbered = true;
        i = j + 1;
    }
    while (i < len) {
        size_t t0, tl, k;
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;
        t0 = i;
        while (i < len && s[i] != ' ') i++;
        tl = i - t0;
        if (tl & 1u) return 0;
        /* Decode each nibble once. ELM responses commonly use compact
         * strings (e.g. 410C0BB8), so this removes a second full token scan. */
        for (k = 0; k < tl; k += 2) {
            int hi = hexval(s[t0 + k]);
            int lo = hexval(s[t0 + k + 1]);
            if (hi < 0 || lo < 0) return 0;
            if (cnt >= cap) return -1;
            out[cnt++] = (uint8_t)((hi << 4) | lo);
        }
    }
    *n = cnt;
    return cnt > 0 ? 1 : 0;
}

static bool try_match(const uint8_t *b, size_t len, const uint8_t *exp, size_t elen,
                      const uint8_t **d, size_t *n)
{
    if (len < elen || memcmp(b, exp, elen) != 0) return false;
    *d = b + elen;
    *n = len - elen;
    return true;
}

/*
 * Find the positive response for expect[] in the collected text. The data
 * after the expect prefix is returned through d/n (points into e->lines or
 * e->payload). Accepted layouts, first match wins (several ECUs may answer):
 *   - ELM numbered multi-frame lines ("0: .." "1: ..") concatenated;
 *   - raw ISO-TP first frame "1L LL .." + "2N .." continuation lines (CAF0
 *     and some clones);
 *   - a single line with the prefix at byte 0, after a single-frame PCI byte
 *     (CAF0), or after an extended address + PCI byte.
 */
static bool resp_match(vp_engine_t *e, const uint8_t *exp, size_t elen, const uint8_t **d,
                       size_t *n)
{
    eng_line_t ln[ENG_MAX_LINES];
    size_t nl = 0, used = 0, k, t;
    const char *s = e->rxbuf;
    const char *end = e->rxbuf + e->rx_len;
    bool any_num = false;

    while (s < end && nl < ENG_MAX_LINES) {
        const char *le = s;
        while (le < end && *le != '\r' && *le != '\n' && *le != '>') le++;
        if (le > s) {
            size_t got = 0;
            bool num = false;
            int r = parse_byte_line(s, (size_t)(le - s), e->lines + used,
                                    sizeof(e->lines) - used, &got, &num);
            if (r > 0) {
                ln[nl].off = (uint16_t)used;
                ln[nl].n = (uint8_t)(got > 255 ? 255 : got);
                ln[nl].num = num;
                any_num |= num;
                used += got;
                nl++;
            }
        }
        s = le + 1;
    }

    if (any_num) {
        t = 0;
        for (k = 0; k < nl; k++) {
            size_t c;
            if (!ln[k].num) continue;
            c = ln[k].n;
            if (t + c > sizeof(e->payload)) c = sizeof(e->payload) - t;
            memcpy(e->payload + t, e->lines + ln[k].off, c);
            t += c;
        }
        if (try_match(e->payload, t, exp, elen, d, n)) return true;
    }

    for (k = 0; k < nl; k++) {
        const uint8_t *L = e->lines + ln[k].off;
        size_t m = ln[k].n;

        if (ln[k].num) continue;
        if (m >= 3 && (L[0] & 0xF0u) == 0x10u) {
            size_t j;
            t = m - 2;
            memcpy(e->payload, L + 2, t);
            for (j = k + 1; j < nl; j++) {
                const uint8_t *C = e->lines + ln[j].off;
                size_t c = ln[j].n;
                if (ln[j].num || c < 2 || (C[0] & 0xF0u) != 0x20u) break;
                c -= 1;
                if (t + c > sizeof(e->payload)) c = sizeof(e->payload) - t;
                memcpy(e->payload + t, C + 1, c);
                t += c;
            }
            if (try_match(e->payload, t, exp, elen, d, n)) return true;
        }
        if (try_match(L, m, exp, elen, d, n)) return true;
        if (m >= 2 && L[0] != 0 && (L[0] & 0xF0u) == 0) {
            size_t pl = L[0] < m - 1 ? L[0] : m - 1;
            if (try_match(L + 1, pl, exp, elen, d, n)) return true;
        }
        if (m >= 3 && L[1] != 0 && (L[1] & 0xF0u) == 0) {
            size_t pl = L[1] < m - 2 ? L[1] : m - 2;
            if (try_match(L + 2, pl, exp, elen, d, n)) return true;
        }
    }
    return false;
}

typedef struct {
    const char *s;
    uint8_t     n;
    uint8_t     hex;
    uint32_t    value;
} eng_tok_t;

/* Space-separated tokens; returns max + 1 when there are more than max.
 * Decode tokens while finding their boundaries: monitor lines otherwise
 * validate every token and then convert the same characters again. */
static size_t tokenize(const char *s, size_t len, eng_tok_t *t, size_t max)
{
    size_t i = 0, nt = 0;
    while (i < len) {
        size_t t0, digits = 0;
        uint32_t value = 0;
        uint8_t hex = 1;
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;
        t0 = i;
        while (i < len && s[i] != ' ') {
            int h = hexval(s[i]);
            if (h < 0) {
                hex = 0;
            } else if (digits < 8) {
                value = (value << 4) | (uint32_t)h;
            }
            digits++;
            i++;
        }
        if (nt >= max) return max + 1;
        t[nt].s = s + t0;
        t[nt].n = (uint8_t)(digits > 255 ? 255 : digits);
        t[nt].hex = hex;
        t[nt].value = value;
        nt++;
    }
    return nt;
}

/*
 * One ATMA line (ATH1, ATS1): "360 00 11 22 ..", "18DAF110 03 ..",
 * "18 DA F1 10 03 ..", optionally "N:"-prefixed or with a DLC digit.
 */
static bool parse_can_line(const char *s, size_t len, uint32_t *id, uint8_t *data, size_t *n)
{
    eng_tok_t t[16];
    size_t nt = tokenize(s, len, t, 16), idx = 0, k;

    if (nt == 0 || nt > 16) return false;
    if (t[0].n >= 2 && t[0].s[t[0].n - 1] == ':') idx = 1;
    for (k = idx; k < nt; k++) {
        if (!t[k].hex || t[k].n == 0) return false;
    }
    if (idx >= nt) return false;
    if (t[idx].n == 3) {
        *id = t[idx].value;
        if (*id > 0x7FFu) return false;
        idx++;
    } else if (t[idx].n == 8) {
        uint32_t v = t[idx].value;
        if (v > VP_ID_MASK) return false;
        *id = v | VP_ID_29BIT;
        idx++;
    } else if (t[idx].n == 2 && nt - idx >= 5 && t[idx + 1].n == 2 && t[idx + 2].n == 2 &&
               t[idx + 3].n == 2) {
        uint32_t v = (t[idx].value << 24) | (t[idx + 1].value << 16) |
                     (t[idx + 2].value << 8) | t[idx + 3].value;
        if (v > VP_ID_MASK) return false;
        *id = v | VP_ID_29BIT;
        idx += 4;
    } else {
        return false;
    }
    if (idx < nt && t[idx].n == 1) {
        idx++;
    } else if (nt - idx == 9 && t[idx].n == 2 && t[idx].value <= 8) {
        idx++;
    }
    if (idx >= nt || nt - idx > 8) return false;
    *n = 0;
    for (k = idx; k < nt; k++) {
        if (t[k].n != 2) return false;
        data[(*n)++] = (uint8_t)t[k].value;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* gear                                                                */
/* ------------------------------------------------------------------ */

static bool gear_band(const vp_profile_t *p, int g, float total, float tol)
{
    float r = p->gears[g - 1] * p->final_drive;
    return total >= r * (1.0f - tol) && total <= r * (1.0f + tol);
}

int8_t vp_engine_gear_from_ratio(const vp_profile_t *p, float rpm, float speed_kmh, int8_t cur)
{
    float total, tol;
    int g;

    if (!p || p->gear_count == 0 || p->final_drive <= 0.0f || p->tire_m <= 0.0f) return cur;
    if (rpm <= 0.0f || speed_kmh <= 0.0f) return 0;
    total = rpm * 0.377f * p->tire_m / speed_kmh;
    tol = p->gear_tol > 0.0f ? p->gear_tol : ENG_DEF_GEAR_TOL;
    if (cur >= 1 && cur <= p->gear_count && gear_band(p, cur, total, tol)) return cur;
    for (g = 1; g <= p->gear_count; g++) {
        if (gear_band(p, g, total, tol)) return (int8_t)g;
    }
    if (rpm > 800.0f && speed_kmh < 5.0f) return 0;
    return cur;
}

static void gear_update(vp_engine_t *e, uint32_t now)
{
    const vp_profile_t *p = e->p;
    const vp_signal_t *s;
    eng_sig_t *g;
    int8_t gear;

    if (e->sig_gear < 0) return;
    s = &e->sig[e->sig_gear];
    g = &e->sigs[e->sig_gear];
    if (g->slot == SIG_SLOT_NONE) return;
    if (!(s->derive == VP_DERIVE_RATIO || (s->fallback == VP_DERIVE_RATIO && g->fallback_on))) return;
    if (!e->rpm_valid || !e->speed_valid || p->gear_count == 0 || p->final_drive <= 0.0f ||
        p->tire_m <= 0.0f) {
        if (e->gear_valid) {
            e->sink.invalidate(e->sink.ctx, g->slot);
            e->gear_valid = false;
        }
        return;
    }
    gear = vp_engine_gear_from_ratio(p, (float)e->rpm, (float)e->speed,
                                     e->gear_valid ? e->gear : 0);
    e->gear = gear;
    e->gear_valid = true;
    put(e, g->slot, gear, now);
}

/* ------------------------------------------------------------------ */
/* signal pipeline                                                     */
/* ------------------------------------------------------------------ */

static void sig_invalidate(vp_engine_t *e, int i, uint32_t now)
{
    int slot = e->sigs[i].slot;

    if (slot == SIG_SLOT_NONE) return;
    e->sink.invalidate(e->sink.ctx, slot);
    if (slot == SIG_OBD_RPM) {
        e->rpm_valid = false;
        gear_update(e, now);
    } else if (slot == SIG_OBD_SPEED) {
        e->speed_valid = false;
        gear_update(e, now);
    } else if (slot == SIG_OBD_GEAR) {
        e->gear_valid = false;
    }
}

static void store(vp_engine_t *e, int i, int32_t v, uint32_t now)
{
    int slot = e->sigs[i].slot;

    e->sigs[i].can_stale = false;
    put(e, slot, v, now);
    if (slot == SIG_OBD_RPM) {
        e->rpm = v;
        e->rpm_valid = true;
        gear_update(e, now);
    } else if (slot == SIG_OBD_SPEED) {
        e->speed = v;
        e->speed_valid = true;
        gear_update(e, now);
    } else if (slot == SIG_OBD_GEAR) {
        e->gear = (int8_t)v;
        e->gear_valid = true;
    }
}

static int16_t f2i(float f)
{
    return (int16_t)(f + 0.5f);
}

/* Legacy oil temperature hysteresis (elm327_ble_client.c default_on_parsed_oil_temp). */
static bool filter_temp_hyst(eng_sig_t *g, int32_t in, bool from_can, int32_t *out)
{
    float fd;
    int diff;

    if (in < -20 || in > 150) return false;
    if (from_can) {
        g->filt = (float)in;
        g->filt_init = true;
        g->hyst_pending = (int16_t)in;
        g->hyst_cnt = 0;
        *out = in;
        return true;
    }
    if (!g->filt_init) {
        g->filt = (float)in;
        g->filt_init = true;
        g->hyst_pending = (int16_t)in;
        g->hyst_cnt = 1;
        *out = in;
        return true;
    }
    fd = g->filt - (float)in;
    diff = (int)(fd >= 0.0f ? fd : -fd);
    if (diff <= 5) {
        g->hyst_pending = ENG_HYST_NONE;
        g->hyst_cnt = 0;
        g->filt = 0.65f * g->filt + 0.35f * (float)in;
        *out = f2i(g->filt);
        return true;
    }
    if (diff <= 15 || diff >= 20) {
        uint8_t need = diff <= 15 ? 4 : 3;
        if (g->hyst_pending == in) {
            g->hyst_cnt++;
        } else {
            g->hyst_pending = (int16_t)in;
            g->hyst_cnt = 1;
            return false;
        }
        if (g->hyst_cnt < need) return false;
        g->filt = diff <= 15 ? 0.5f * g->filt + 0.5f * (float)in : (float)in;
        g->hyst_pending = ENG_HYST_NONE;
        g->hyst_cnt = 0;
        *out = diff <= 15 ? f2i(g->filt) : in;
        return true;
    }
    return false;
}

static int32_t filter_ema(eng_sig_t *g, int32_t in)
{
    if (!g->filt_init) {
        g->filt = (float)in;
        g->filt_init = true;
    } else {
        g->filt += ENG_EMA_ALPHA * ((float)in - g->filt);
    }
    return (int32_t)floorf(g->filt + 0.5f);
}

/* Round Binary32 like floor((double)v + 0.5), using hardware integer/float
 * conversions. Guard the cast first; fractional inputs are too small for the
 * final +/-1 to overflow. Negative half ties round toward positive infinity. */
static bool round_identity(float v, int32_t *out)
{
    int32_t integer;
    float fraction;
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return false;
    integer = (int32_t)v;
    fraction = v - (float)integer;
    if (fraction >= 0.5f) integer++;
    else if (fraction < -0.5f) integer--;
    *out = integer;
    return true;
}

/* P6..P10 for a source value v (P1..P5 done by the caller). */
static void write_value(vp_engine_t *e, int i, float v, uint32_t now, bool from_can)
{
    const vp_signal_t *s = &e->sig[i];
    eng_sig_t *g = &e->sigs[i];
    double x;
    int32_t internal;

    if (g->slot == SIG_OBD_SPEED && e->p->speed_scale > 0.0f) v *= e->p->speed_scale;
    /* P6 post: clamp -> zero_if_rpm_lt -> zero_le */
    if (s->flags & VP_SIG_F_CLAMP) {
        if (v < s->clamp_lo) v = s->clamp_lo;
        if (v > s->clamp_hi) v = s->clamp_hi;
    }
    if (s->flags & VP_SIG_F_ZERO_RPM) {
        int32_t rpm = e->rpm_valid ? e->rpm : 0;
        if (rpm < (int32_t)s->zero_if_rpm_lt) v = 0.0f;
    }
    if ((s->flags & VP_SIG_F_ZERO_LE) && v <= s->zero_le) v = 0.0f;
    /* P7 signal valid */
    if ((s->flags & VP_SIG_F_VALID) && (v < s->valid_lo || v > s->valid_hi)) {
        sig_invalidate(e, i, now);
        return;
    }
    /* P8 internal units. Most standard signals use an identity factor; keep
     * that hot path in single precision and avoid the Xtensa double helpers.
     * The general path preserves the profile's double-precision rounding. */
    if (e->factor_identity_mask & (UINT64_C(1) << (unsigned)i)) {
        if (!round_identity(v, &internal)) {
            sig_invalidate(e, i, now);
            return;
        }
    } else {
        x = floor((double)v * g->factor + 0.5);
        if (!(x >= -2147483648.0 && x <= 2147483647.0)) {
            sig_invalidate(e, i, now);
            return;
        }
        internal = (int32_t)x;
    }
    /* P9 registry range (veh.* ranges are enforced by signal_store) */
    if (g->slot < SIG_OBD_COUNT &&
        (internal < sig_registry[g->slot].min || internal > sig_registry[g->slot].max)) {
        sig_invalidate(e, i, now);
        return;
    }
    /* P10 filter + write */
    if (s->filter == VP_FILTER_TEMP_HYST) {
        if (!filter_temp_hyst(g, internal, from_can, &internal)) return;
    } else if (s->filter == VP_FILTER_EMA) {
        internal = filter_ema(g, internal);
    }
    store(e, i, internal, now);
}

static void source_exhausted(vp_engine_t *e, int i, uint32_t now)
{
    e->sigs[i].exhausted = true;
    if (i == e->sig_gear && e->sig[i].fallback == VP_DERIVE_RATIO) {
        e->sigs[i].fallback_on = true;
        gear_update(e, now);
        return;
    }
    sig_invalidate(e, i, now);
}

/* Move to the next usable source (CAN sources are skipped without a monitor). */
static void advance_source(vp_engine_t *e, int i, uint32_t now)
{
    const vp_signal_t *s = &e->sig[i];
    eng_sig_t *g = &e->sigs[i];
    bool wrapped = false;
    uint8_t k;

    g->fails = 0;
    for (k = 0; k < s->src_count; k++) {
        g->cur++;
        if (g->cur >= s->src_count) {
            g->cur = 0;
            wrapped = true;
        }
        if (e->src[s->src_first + g->cur].kind != VP_SRC_CAN || mon_usable(e)) break;
    }
    if (wrapped) source_exhausted(e, i, now);
}

/* P5: one failed sample of signal i's current source. */
static void sig_fail(vp_engine_t *e, int i, uint32_t now)
{
    const vp_source_t *s = cur_src(e, i);
    eng_sig_t *g = &e->sigs[i];
    uint8_t fa;

    if (!s) return;
    fa = s->fail_after ? s->fail_after : VP_DEFAULT_FAIL_AFTER;
    if (++g->fails < fa) return;
    advance_source(e, i, now);
}

/* P1 len check + P2 RPN + P3 source valid. */
static bool src_eval(vp_engine_t *e, uint16_t si, const uint8_t *d, size_t n, float *v)
{
    const vp_source_t *s = &e->src[si];

    if (s->kind == VP_SRC_REQ && n < s->len) return false;
    return vp_profile_eval_source(e->p, si, d, n, v) == VP_RPN_OK;
}

/* P4 hold. */
static float apply_hold(vp_engine_t *e, uint16_t si, float v, uint32_t now)
{
    const vp_source_t *s = &e->src[si];
    eng_hold_t *h = &e->holds[si];

    if (!(s->flags & VP_SRC_F_HOLD)) return v;
    if (!h->has || (uint32_t)(now - h->last_ms) > s->hold_gap_ms ||
        fabsf(v - h->last) <= s->hold_delta) {
        h->has = true;
        h->last = v;
        h->last_ms = now;
        h->held = 0;
        return v;
    }
    if (h->held < s->hold_max) {
        h->held++;
        return h->last;
    }
    h->last = v;
    h->last_ms = now;
    h->held = 0;
    return v;
}

static void sample_ok(vp_engine_t *e, int i, uint16_t si, float v, uint32_t now, bool from_can)
{
    eng_sig_t *g = &e->sigs[i];

    v = apply_hold(e, si, v, now);
    g->fails = 0;
    g->exhausted = false;
    g->fallback_on = false;
    e->last_good_ms = now;
    e->heal_count = 0;
    if (e->sink.on_response) e->sink.on_response(e->sink.ctx, g->slot);
    write_value(e, i, v, now, from_can);
}

/* ------------------------------------------------------------------ */
/* request / frame handling                                            */
/* ------------------------------------------------------------------ */

static bool sig_driven(const vp_engine_t *e, int i)
{
    return e->sigs[i].slot != SIG_SLOT_NONE && e->sig[i].derive == VP_DERIVE_NONE &&
           e->sig[i].src_count > 0;
}

static void req_index_append(vp_engine_t *e, uint16_t r, uint16_t si)
{
    uint8_t source = (uint8_t)si;

    if (r >= e->p->n_req || si >= e->p->n_src) return;
    e->req_next[source] = ENG_INDEX_NONE;
    if (e->req_head[r] == ENG_INDEX_NONE) {
        e->req_head[r] = source;
    } else {
        e->req_next[e->req_tail[r]] = source;
    }
    e->req_tail[r] = source;
}

static void req_done(vp_engine_t *e, int r, bool answered, uint32_t now)
{
    const vp_request_t *q = &e->req[r];
    const uint8_t *d = NULL;
    size_t n = 0;
    bool match = answered && resp_match(e, q->expect, q->expect_len, &d, &n);
    uint8_t source;

    if (match) e->st.rx_ok++;
    else e->st.rx_fail++;
    for (source = e->req_head[r]; source != ENG_INDEX_NONE; source = e->req_next[source]) {
        const vp_source_t *s = &e->src[source];
        int i = (int)s->sig;
        uint16_t si;
        float v;

        if (i < 0 || i >= e->p->n_sig || !sig_driven(e, i)) continue;
        si = (uint16_t)(e->sig[i].src_first + e->sigs[i].cur);
        if (si != source) continue;
        if (match && src_eval(e, si, d, n, &v)) {
            sample_ok(e, i, si, v, now, false);
        } else {
            sig_fail(e, i, now);
        }
    }
}

static void can_frame(vp_engine_t *e, uint32_t id, const uint8_t *d, size_t n, uint32_t now)
{
    uint16_t k;

    e->st.can_frames++;
    for (k = 0; k < e->p->n_can; k++) {
        const vp_can_rule_t *rule = &e->can[k];
        uint16_t si = rule->src, cur_abs;
        int i = rule->sig;
        float v;

        if (rule->can_id != id || i >= e->p->n_sig || !sig_driven(e, i)) continue;
        cur_abs = (uint16_t)(e->sig[i].src_first + e->sigs[i].cur);
        if (si > cur_abs) continue;
        if (!src_eval(e, si, d, n, &v)) {
            if (si == cur_abs) sig_fail(e, i, now);
            continue;
        }
        if (si < cur_abs) {
            e->sigs[i].cur = (uint8_t)(si - e->sig[i].src_first);
            e->sigs[i].fails = 0;
        }
        e->sigs[i].can_seen = true;
        e->sigs[i].last_can_ms = now;
        sample_ok(e, i, si, v, now, true);
    }
}

static void mon_line(vp_engine_t *e, const char *s, size_t len, uint32_t now)
{
    uint8_t data[8];
    size_t n = 0;
    uint32_t id = 0;

    while (len && s[0] == ' ') {
        s++;
        len--;
    }
    while (len && s[len - 1] == ' ') len--;
    if (len == 0) return;
    if (len == 1 && s[0] == '?') {
        e->ma_refused = true;
        return;
    }
    if (parse_can_line(s, len, &id, data, &n)) can_frame(e, id, data, n, now);
}

/* Consume complete monitor lines from the receive buffer. */
static void mon_consume(vp_engine_t *e, uint32_t now, bool flush)
{
    size_t start = 0, i;

    for (i = 0; i < e->rx_len; i++) {
        char c = e->rxbuf[i];
        if (c == '\r' || c == '\n' || c == '>') {
            if (i > start) mon_line(e, &e->rxbuf[start], i - start, now);
            start = i + 1;
        }
    }
    if (flush && start < e->rx_len) {
        mon_line(e, &e->rxbuf[start], e->rx_len - start, now);
        start = e->rx_len;
    }
    if (start == 0 && e->rx_len >= ENG_RX_CAP) start = e->rx_len;   /* garbage without newlines */
    if (start) {
        size_t remain = e->rx_len - start;
        if (remain) memmove(e->rxbuf, e->rxbuf + start, remain);
        e->rx_len = (uint16_t)remain;
    }
}

static void ma_done(vp_engine_t *e, uint32_t now)
{
    int i;

    mon_consume(e, now, true);
    if (e->ma_refused) {
        elog(e, "ATMA refused, monitor disabled for this link");
        e->mon_ok = false;
        for (i = 0; i < e->p->n_sig; i++) {
            const vp_source_t *s;
            if (!sig_driven(e, i)) continue;
            s = cur_src(e, i);
            if (s->kind == VP_SRC_CAN) advance_source(e, i, now);
        }
        return;
    }
    for (i = 0; i < e->p->n_sig; i++) {
        eng_sig_t *g = &e->sigs[i];
        const vp_source_t *s;
        if (!sig_driven(e, i)) continue;
        s = cur_src(e, i);
        if (s->kind != VP_SRC_CAN) {
            g->can_seen = false;
            continue;
        }
        if (!g->can_seen) sig_fail(e, i, now);
        g->can_seen = false;
        /* sig_fail may have moved to a request source */
        if (cur_src(e, i)->kind == VP_SRC_CAN && e->p->mon_stale_ms && !g->can_stale &&
            elapsed(now, g->last_can_ms, e->p->mon_stale_ms)) {
            sig_invalidate(e, i, now);
            g->can_stale = true;
        }
    }
}

static void rv_done(vp_engine_t *e)
{
    const char *s = e->rxbuf;
    double v;

    while (*s && !(*s >= '0' && *s <= '9')) s++;
    if (!*s) return;
    v = strtod(s, NULL);
    if (v > 0.0 && v < 40.0 && e->sink.on_voltage) e->sink.on_voltage(e->sink.ctx, (float)v);
}

/* ------------------------------------------------------------------ */
/* bring-up                                                            */
/* ------------------------------------------------------------------ */

static void start_init(vp_engine_t *e)
{
    const vp_profile_t *p = e->p;
    const vp_at_cmd_t *init = vp_profile_init(p);
    static const char *const k_base[] = { "ATZ", "ATE0", "ATL0", "ATS1", "ATH0", "ATAT1" };
    char c[ENG_CMD_CAP];
    eng_step_t *s;
    size_t k;
    uint16_t i;

    e->phase = VP_ENGINE_PH_INIT;
    e->st.phase = e->phase;
    q_clear(e);
    /* state after ATZ */
    e->hdr = ENG_ID_UNKNOWN;
    e->rx = 0;
    e->base_rx = 0;
    e->cea = ENG_CEA_OFF;
    e->caf0 = 0;
    for (k = 0; k < sizeof(k_base) / sizeof(k_base[0]); k++) q_push(e, K_AT, k_base[k], ENG_AT_DELAY);
    snprintf(c, sizeof(c), "ATST%02X", (unsigned)(p->atst >= 2 ? p->atst : VP_DEFAULT_ATST));
    q_push(e, K_AT, c, ENG_AT_DELAY);
    snprintf(c, sizeof(c), "ATSP%X", (unsigned)e->run_proto);
    q_push(e, K_AT, c, ENG_AT_DELAY);
    push_state(e, ST_HDR, p->hdr, -1);
    if (e->q_n) e->q[(e->q_head + e->q_n - 1) % ENG_STEPS].delay_ms = ENG_AT_DELAY;
    for (i = 0; i < p->n_init; i++) q_push(e, K_INIT, init[i].cmd, ENG_AT_DELAY);
    for (k = 0; k < ENG_WARM_COUNT; k++) q_push(e, K_WARM, "0100", ENG_WARM_DELAY);
    s = q_push(e, K_RV, "ATRV", 0);
    (void)s;
    elog(e, "init %s proto %u", p->id, (unsigned)e->run_proto);
}

static void start_detect(vp_engine_t *e)
{
    e->phase = VP_ENGINE_PH_DETECT;
    e->st.phase = e->phase;
    e->detect_next = 1;
    q_clear(e);
    q_push(e, K_AT, "ATZ", ENG_AT_DELAY);
    q_push(e, K_AT, "ATE0", ENG_AT_DELAY);
    q_push(e, K_AT, "ATAT1", ENG_AT_DELAY);
    q_push(e, K_AT, "ATST19", ENG_DETECT_SETTLE);
    elog(e, "protocol auto-detect");
}

static void detect_next(vp_engine_t *e)
{
    char c[ENG_CMD_CAP];
    eng_step_t *s;

    if (e->detect_next > ENG_DETECT_LAST) {
        elog(e, "no protocol answered, using %d", ENG_DETECT_FALLBACK);
        e->run_proto = ENG_DETECT_FALLBACK;
        start_init(e);
        return;
    }
    snprintf(c, sizeof(c), "ATSP%X", (unsigned)e->detect_next);
    q_push(e, K_AT, c, ENG_DETECT_SP_DELAY);
    s = q_push(e, K_DETECT, "010C", 0);
    if (s) {
        s->timeout_ms = e->cfg.detect_timeout_ms ? e->cfg.detect_timeout_ms : ENG_DEF_DETECT;
        s->val = e->detect_next;
    }
    e->detect_next++;
}

static void detect_done(vp_engine_t *e, const eng_step_t *s, bool answered)
{
    static const uint8_t k_exp[2] = { 0x41, 0x0C };
    const uint8_t *d;
    size_t n;

    if (!answered || !resp_match(e, k_exp, sizeof(k_exp), &d, &n)) return;
    e->det_proto = (uint8_t)s->val;
    e->run_proto = (uint8_t)s->val;
    elog(e, "protocol %u detected", (unsigned)s->val);
    if (e->sink.on_proto_detected) e->sink.on_proto_detected(e->sink.ctx, (uint8_t)s->val);
    start_init(e);
}

static void init_cmd_done(vp_engine_t *e, const eng_step_t *s, bool ok)
{
    const char *c = s->cmd;
    uint32_t id = 0;

    if (!strncmp(c, "ATSH", 4)) {
        e->hdr = (ok && vp_classify_tx_header(c + 4, &id) != VP_HDR_INVALID) ? id : ENG_ID_UNKNOWN;
    } else if (!strncmp(c, "ATCRA", 5)) {
        if (c[5] == '\0' || !vp_parse_rx_filter(c + 5, &id)) id = 0;
        e->base_rx = id;
        e->rx = ok ? id : ENG_ID_UNKNOWN;
    } else if (!strcmp(c, "ATCAF1")) {
        e->caf0 = ok ? 0 : ENG_ST_UNKNOWN;
    }
}

static void enter_run(vp_engine_t *e, uint32_t now)
{
    int i;

    e->phase = VP_ENGINE_PH_RUN;
    e->st.phase = e->phase;
    e->last_good_ms = now;
    e->last_vbat_ms = now;
    e->last_mon_ms = now;
    e->in_round = false;
    e->slot_n = e->slot_pos = 0;
    for (i = 0; i < e->p->n_sig; i++) {
        e->sigs[i].last_poll_ms = now - 60000u;
        e->sigs[i].last_can_ms = now;
    }
    elog(e, "running");
}

/* ------------------------------------------------------------------ */
/* polling plan                                                        */
/* ------------------------------------------------------------------ */

static bool poll_candidate(const vp_engine_t *e, int i)
{
    if (!sig_driven(e, i) || e->sig[i].rate == VP_RATE_EVERY) return false;
    return cur_src(e, i)->kind == VP_SRC_REQ;
}

static bool sig_due(const vp_engine_t *e, int i, uint32_t now)
{
    const vp_signal_t *s = &e->sig[i];

    if (s->period_ms && !elapsed(now, e->sigs[i].last_poll_ms, s->period_ms)) return false;
    if (s->rate == VP_RATE_SLOW) {
        uint32_t div = s->slow_div ? s->slow_div : VP_DEFAULT_SLOW_DIV;
        return ((e->round_no + (uint32_t)i) % div) == 0;
    }
    return true;
}

static size_t every_reqs(const vp_engine_t *e, uint8_t *out)
{
    size_t n = 0, k, j;
    for (k = 0; k < e->n_every; k++) {
        int i = e->every_sig[k];
        const vp_source_t *s;
        if (!sig_driven(e, i)) continue;
        s = cur_src(e, i);
        if (s->kind != VP_SRC_REQ) continue;
        for (j = 0; j < n && out[j] != s->ref; j++) {}
        if (j == n) out[n++] = (uint8_t)s->ref;
    }
    return n;
}

static void start_round(vp_engine_t *e, uint32_t now)
{
    uint8_t ev[VP_MAX_EVERY_SIGNALS];
    uint8_t k;
    uint32_t seen = 0;

    /* Validated request refs are 0..31. Keep first-seen profile order in the
     * array; the local mask only replaces its repeated membership scans. */
    e->round_n = 0;
    e->round_pos = 0;
    e->every_done = false;
    for (k = 0; k < e->n_poll; k++) {
        int i = e->poll_sig[k];
        uint8_t r;
        uint32_t bit;
        if (!poll_candidate(e, i) || !sig_due(e, i, now)) continue;
        r = (uint8_t)cur_src(e, i)->ref;
        bit = UINT32_C(1) << r;
        if (!(seen & bit)) {
            seen |= bit;
            e->round_req[e->round_n++] = r;
        }
        /* A shared request still advances each due signal's deadline. */
        e->sigs[i].last_poll_ms = now;
    }
    e->in_round = e->round_n > 0 || every_reqs(e, ev) > 0;
}

static bool build_slot(vp_engine_t *e)
{
    uint8_t ev[VP_MAX_EVERY_SIGNALS];
    size_t nev = every_reqs(e, ev), k;

    e->slot_n = 0;
    e->slot_pos = 0;
    if (e->round_pos < e->round_n) {
        uint8_t main_req = e->round_req[e->round_pos++];
        e->slot_req[e->slot_n++] = main_req;
        for (k = 0; k < nev; k++) {
            if (ev[k] != main_req) e->slot_req[e->slot_n++] = ev[k];
        }
    } else if (e->round_n == 0 && !e->every_done) {
        e->every_done = true;
        for (k = 0; k < nev; k++) e->slot_req[e->slot_n++] = ev[k];
    }
    return e->slot_n > 0;
}

/* Queue the first adapter-state change request r needs; false when none. */
static bool push_state_for(vp_engine_t *e, uint8_t r)
{
    const vp_request_t *q = &e->req[r];
    uint32_t want_rx = (q->flags & VP_REQ_F_RX) ? q->rx : e->base_rx;
    int16_t want_cea = (q->flags & VP_REQ_F_CEA) ? (int16_t)q->cea : (int16_t)ENG_CEA_OFF;
    int8_t want_caf0 = (q->flags & VP_REQ_F_RAW) ? 1 : 0;

    if (e->hdr != q->hdr) {
        push_state(e, ST_HDR, q->hdr, r);
    } else if (e->rx != want_rx) {
        push_state(e, ST_RX, want_rx, r);
    } else if (e->cea != want_cea) {
        push_state(e, ST_CEA, want_cea == ENG_CEA_OFF ? ENG_CEA_OFF_VAL : (uint32_t)want_cea, r);
    } else if (e->caf0 != want_caf0) {
        push_state(e, ST_CAF, (uint32_t)want_caf0, r);
    } else {
        return false;
    }
    return true;
}

static void push_req(vp_engine_t *e, uint8_t r)
{
    const vp_request_t *q = &e->req[r];
    char c[ENG_CMD_CAP];
    eng_step_t *s;
    uint8_t k, n = q->tx_len < 8 ? q->tx_len : 8;

    // This runs on every OBD poll. Format bytes directly instead of invoking
    // snprintf once per byte (up to eight formatter calls per request).
    for (k = 0; k < n; k++) {
        c[2u * k] = eng_hex[q->tx[k] >> 4];
        c[2u * k + 1u] = eng_hex[q->tx[k] & 0x0Fu];
    }
    c[2u * n] = '\0';
    s = q_push(e, K_REQ, c, e->p->gap_ms);
    if (s) s->req = r;
}

static void start_monitor(vp_engine_t *e, uint32_t now)
{
    e->last_mon_ms = now;
    e->st.mon_windows++;
    if (e->caf0 != 0) push_state(e, ST_CAF, 0, -1);
    if (e->cea != ENG_CEA_OFF) push_state(e, ST_CEA, ENG_CEA_OFF_VAL, -1);
    if (e->rx != 0) push_state(e, ST_RX, 0, -1);
    q_push(e, K_AT, "ATH1", 0);
    q_push(e, K_MA, "ATMA", 0);
    q_push(e, K_AT, "ATAR", 0);
    q_push(e, K_AT, "ATH0", 0);
}

static bool mon_due(const vp_engine_t *e, uint32_t now)
{
    if (!mon_usable(e)) return false;
    if (e->p->mon_mode == VP_MON_CONTINUOUS) return true;
    return elapsed(now, e->last_mon_ms, e->p->mon_every_ms);
}

static void heal(vp_engine_t *e, uint32_t now)
{
    e->st.heals++;
    e->heal_count++;
    elog(e, "no data for %u ms, heal %u", (unsigned)heal_after(e), (unsigned)e->heal_count);
    if (e->heal_count >= ENG_HEAL_RECONNECT) {
        e->heal_count = 0;
        if (e->sink.reconnect) {
            q_clear(e);
            e->phase = VP_ENGINE_PH_RECONNECT;
            e->st.phase = e->phase;
            e->sink.reconnect(e->sink.ctx);
            return;
        }
    }
    e->last_good_ms = now;
    start_init(e);
}

static void plan_run(vp_engine_t *e, uint32_t now)
{
    int guard;

    if (elapsed(now, e->last_good_ms, heal_after(e))) {
        heal(e, now);
        return;
    }
    for (guard = 0; guard < 4; guard++) {
        if (e->slot_pos < e->slot_n) {
            uint8_t r = e->slot_req[e->slot_pos];
            if (!push_state_for(e, r)) {
                push_req(e, r);
                e->slot_pos++;
            }
            return;
        }
        if (e->in_round) {
            if (build_slot(e)) continue;
            e->in_round = false;
            e->round_no++;
            e->st.rounds++;
        }
        if (elapsed(now, e->last_vbat_ms, vbat_every(e))) {
            e->last_vbat_ms = now;
            q_push(e, K_RV, "ATRV", 0);
            return;
        }
        if (mon_due(e, now)) {
            start_monitor(e, now);
            return;
        }
        start_round(e, now);
        if (!e->in_round) {
            e->round_no++;
            e->next_ms = now + ENG_IDLE_MS;
            return;
        }
    }
}

static void plan(vp_engine_t *e, uint32_t now)
{
    switch (e->phase) {
    case VP_ENGINE_PH_DETECT:
        detect_next(e);
        break;
    case VP_ENGINE_PH_INIT:
        enter_run(e, now);
        break;
    case VP_ENGINE_PH_RUN:
        plan_run(e, now);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* step execution                                                      */
/* ------------------------------------------------------------------ */

static bool resp_ok(const vp_engine_t *e)
{
    return strstr(e->rxbuf, "OK") != NULL;
}

static void step_done(vp_engine_t *e, uint32_t now, int how)
{
    eng_step_t s = e->cur;
    bool answered = how == DONE_PROMPT;

    e->busy = false;
    e->rxbuf[e->rx_len] = '\0';
    if (how == DONE_TIMEOUT && s.kind != K_MA) e->st.timeouts++;
    switch (s.kind) {
    case K_STATE: {
        bool ok = answered && resp_ok(e);
        switch (s.st) {
        case ST_HDR: e->hdr = ok ? s.val : ENG_ID_UNKNOWN; break;
        case ST_RX:  e->rx = ok ? s.val : ENG_ID_UNKNOWN; break;
        case ST_CEA:
            e->cea = ok ? (s.val == ENG_CEA_OFF_VAL ? (int16_t)ENG_CEA_OFF : (int16_t)s.val)
                        : (int16_t)ENG_ST_UNKNOWN;
            break;
        default:     e->caf0 = ok ? (int8_t)s.val : (int8_t)ENG_ST_UNKNOWN; break;
        }
        if (!ok) {
            elog(e, "%s failed", s.cmd);
            if (s.req >= 0) {
                /* the request cannot go out with the wrong framing: count it failed */
                if (e->slot_pos < e->slot_n && e->slot_req[e->slot_pos] == s.req) e->slot_pos++;
                req_done(e, s.req, false, now);
            }
        }
        break;
    }
    case K_INIT:
        init_cmd_done(e, &s, answered && resp_ok(e));
        break;
    case K_AT:
        if (!strcmp(s.cmd, "ATAR")) e->rx = answered && resp_ok(e) ? 0 : ENG_ID_UNKNOWN;
        break;
    case K_DETECT:
        detect_done(e, &s, answered);
        break;
    case K_RV:
        if (answered) rv_done(e);
        break;
    case K_REQ:
        req_done(e, s.req, answered, now);
        break;
    case K_MA:
        ma_done(e, now);
        break;
    default:
        break;
    }
    e->next_ms = now + s.delay_ms;
    e->rx_len = 0;
    e->rxbuf[0] = '\0';
    e->prompt = false;
}

static void step_send(vp_engine_t *e, const eng_step_t *s, uint32_t now)
{
    char norm[VP_AT_MAX_LEN + 1];
    const char *cmd = s->cmd;
    bool is_at = s->kind != K_REQ && s->kind != K_DETECT && s->kind != K_WARM;

    e->cur = *s;
    e->rx_len = 0;
    e->rxbuf[0] = '\0';
    e->prompt = false;
    e->busy = true;
    e->sent_ms = now;
    if (is_at) {
        const char *why = NULL;
        if (!vp_at_check(s->cmd, VP_AT_CTX_ENGINE, norm, sizeof(norm), &why)) {
            elog(e, "AT refused: %s (%s)", s->cmd, why ? why : "?");
            step_done(e, now, DONE_FAIL);
            return;
        }
        cmd = norm;
    }
    if (s->kind == K_REQ) {
        e->req_sent = true;
        e->last_req_ms = now;
        e->st.tx_req++;
    }
    if (s->kind == K_MA) {
        uint32_t w = e->p->mon_mode == VP_MON_CONTINUOUS ? ENG_CONT_WINDOW_MS : e->p->mon_window_ms;
        e->ma_stopping = false;
        e->ma_refused = false;
        e->ma_end_ms = now + w;
    }
    e->st.tx_cmds++;
    if (!e->sink.send(e->sink.ctx, cmd)) {
        elog(e, "send failed: %s", cmd);
        step_done(e, now, DONE_FAIL);
    }
}

/* Monitor window in flight: false while still listening (*wait set). */
static bool ma_poll(vp_engine_t *e, uint32_t now, uint32_t *wait)
{
    if (e->prompt) {
        step_done(e, now, DONE_PROMPT);
        return true;
    }
    if ((int32_t)(e->ma_end_ms - now) > 0) {
        *wait = cap_wait(e->ma_end_ms - now);
        return false;
    }
    if (!e->ma_stopping) {
        /* any character ends ATMA; the prompt follows */
        e->ma_stopping = true;
        e->ma_end_ms = now + ENG_MA_STOP_MS;
        e->st.tx_cmds++;
        e->sink.send(e->sink.ctx, "");
        *wait = ENG_MA_STOP_MS;
        return false;
    }
    step_done(e, now, DONE_TIMEOUT);
    return true;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

static int registry_slot(const char *key)
{
    int k;
    for (k = 0; k < SIG_OBD_COUNT; k++) {
        if (!strcmp(sig_registry[SIG_OBD_FIRST + k].key, key)) return SIG_OBD_FIRST + k;
    }
    return SIG_SLOT_NONE;
}

static double unit_factor(int slot, const char *unit)
{
    const sig_def_t *d;
    uint8_t k;

    if (slot < 0 || slot >= SIG_STD_COUNT) return 1.0;
    d = &sig_registry[slot];
    if (!d->units || d->n_units == 0) return 1.0;
    if (unit && unit[0]) {
        for (k = 0; k < d->n_units; k++) {
            if (!strcmp(d->units[k].unit, unit)) return d->units[k].factor;
        }
    }
    return d->units[0].factor;
}

static void veh_range(const vp_signal_t *s, int32_t *lo, int32_t *hi)
{
    double a = -2147483648.0, b = 2147483647.0;

    if (s->flags & VP_SIG_F_VALID) {
        a = floor(s->valid_lo);
        b = ceil(s->valid_hi);
    } else if (s->flags & VP_SIG_F_CLAMP) {
        a = floor(s->clamp_lo);
        b = ceil(s->clamp_hi);
    }
    if (a < -2147483648.0) a = -2147483648.0;
    if (b > 2147483647.0) b = 2147483647.0;
    *lo = (int32_t)a;
    *hi = (int32_t)b;
}

static uint32_t veh_stale(const vp_signal_t *s)
{
    if (s->rate == VP_RATE_SLOW) return 60000u;
    if (s->period_ms > 5000u) {
        uint32_t t = 3u * s->period_ms;
        return t > 60000u ? 60000u : t;
    }
    return 15000u;
}

static void reset_runtime(vp_engine_t *e, uint32_t now)
{
    int i;

    q_clear(e);
    e->busy = false;
    e->prompt = false;
    e->rx_len = 0;
    e->rxbuf[0] = '\0';
    e->next_ms = now;
    e->hdr = ENG_ID_UNKNOWN;
    e->rx = ENG_ID_UNKNOWN;
    e->base_rx = 0;
    e->cea = ENG_ST_UNKNOWN;
    e->caf0 = ENG_ST_UNKNOWN;
    e->in_round = false;
    e->round_n = e->round_pos = 0;
    e->slot_n = e->slot_pos = 0;
    e->round_no = 0;
    e->req_sent = false;
    e->mon_ok = true;
    e->heal_count = 0;
    e->rpm_valid = e->speed_valid = e->gear_valid = false;
    e->gear = 0;
    e->last_data_ms = e->last_good_ms = now;
    for (i = 0; i < e->p->n_sig; i++) {
        eng_sig_t *g = &e->sigs[i];
        g->cur = 0;
        g->fails = 0;
        g->exhausted = false;
        g->fallback_on = false;
        g->can_seen = false;
        g->can_stale = false;
        g->filt_init = false;
        g->hyst_cnt = 0;
        g->hyst_pending = ENG_HYST_NONE;
        g->filt = 0.0f;
        g->last_poll_ms = now;
        g->last_can_ms = now;
    }
    memset(e->holds, 0, sizeof(e->holds));
}

vp_engine_t *vp_engine_create(const vp_profile_t *p, const vp_engine_sink_t *sink,
                              const vp_engine_cfg_t *cfg, const vp_allocator_t *alloc)
{
    vp_engine_t *e;
    int i;

    if (!p || !sink || !alloc || !alloc->alloc || !alloc->free) return NULL;
    if (!sink->send || !sink->now_ms || !sink->set_signal || !sink->invalidate ||
        !sink->set_speed_kmh || !sink->on_link) {
        return NULL;
    }
    if (p->n_sig > VP_MAX_SIGNALS || p->n_src > VP_MAX_SOURCES || p->n_req > VP_MAX_REQUESTS) {
        return NULL;
    }
    e = (vp_engine_t *)alloc->alloc(alloc->ctx, sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->p = p;
    e->req = vp_profile_requests(p);
    e->sig = vp_profile_signals(p);
    e->src = vp_profile_sources(p);
    e->can = vp_profile_can_rules(p);
    e->sink = *sink;
    if (cfg) e->cfg = *cfg;
    if (e->cfg.proto > 12) e->cfg.proto = 0;
    e->alloc = *alloc;
    e->phase = VP_ENGINE_PH_DOWN;
    e->link = SIG_LINK_NONE;
    e->mon_ok = true;
    e->sig_gear = -1;
    memset(e->req_head, ENG_INDEX_NONE, sizeof(e->req_head));
    memset(e->req_tail, ENG_INDEX_NONE, sizeof(e->req_tail));
    memset(e->req_next, ENG_INDEX_NONE, sizeof(e->req_next));

    for (i = 0; i < p->n_sig; i++) {
        const vp_signal_t *s = &e->sig[i];
        eng_sig_t *g = &e->sigs[i];

        g->slot = SIG_SLOT_NONE;
        g->factor = 1.0;
        if (s->src_first + s->src_count > p->n_src) continue;
        if (s->flags & VP_SIG_F_VEH) {
            int32_t lo, hi;
            veh_range(s, &lo, &hi);
            if (sink->veh_register) {
                int slot = sink->veh_register(sink->ctx, s->key, lo, hi, veh_stale(s));
                g->slot = (int16_t)(slot < 0 ? SIG_SLOT_NONE : slot);
            }
        } else {
            int slot = registry_slot(s->key);
            g->slot = (int16_t)slot;
            g->factor = unit_factor(slot, s->unit);
            if (slot == SIG_OBD_GEAR) e->sig_gear = (int16_t)i;
        }
        if (g->slot == SIG_SLOT_NONE) {
            elog(e, "signal %s not mapped", s->key);
            continue;
        }
        if (g->factor == 1.0) e->factor_identity_mask |= UINT64_C(1) << (unsigned)i;
        if (s->derive == VP_DERIVE_NONE) {
            uint8_t k;
            bool has_request = false;
            for (k = 0; k < s->src_count; k++) {
                uint16_t si = (uint16_t)(s->src_first + k);
                if (e->src[si].kind == VP_SRC_REQ) {
                    req_index_append(e, e->src[si].ref, si);
                    has_request = true;
                }
            }
            if (has_request && e->n_poll < VP_MAX_SIGNALS) {
                e->poll_sig[e->n_poll++] = (uint8_t)i;
            }
        }
        if (s->rate == VP_RATE_EVERY && s->derive == VP_DERIVE_NONE &&
            e->n_every < VP_MAX_EVERY_SIGNALS) {
            e->every_sig[e->n_every++] = (uint8_t)i;
        }
    }
    reset_runtime(e, sink->now_ms(sink->ctx));
    return e;
}

void vp_engine_destroy(vp_engine_t *e)
{
    vp_allocator_t a;

    if (!e) return;
    if (e->sink.veh_clear) e->sink.veh_clear(e->sink.ctx);
    a = e->alloc;
    a.free(a.ctx, e);
}

void vp_engine_link_up(vp_engine_t *e)
{
    uint32_t now;
    uint8_t pr;

    if (!e) return;
    now = e->sink.now_ms(e->sink.ctx);
    reset_runtime(e, now);
    set_link(e, SIG_LINK_NO_DATA);
    if (e->sink.on_elm_link) e->sink.on_elm_link(e->sink.ctx, true);
    pr = e->p->proto ? e->p->proto : (e->det_proto ? e->det_proto : e->cfg.proto);
    if (pr == 0) {
        start_detect(e);
    } else {
        e->run_proto = pr;
        start_init(e);
    }
}

void vp_engine_link_down(vp_engine_t *e)
{
    int i;

    if (!e) return;
    for (i = 0; i < e->p->n_sig; i++) {
        if (e->sigs[i].slot != SIG_SLOT_NONE) e->sink.invalidate(e->sink.ctx, e->sigs[i].slot);
    }
    e->rpm_valid = e->speed_valid = e->gear_valid = false;
    q_clear(e);
    e->busy = false;
    e->phase = VP_ENGINE_PH_DOWN;
    e->st.phase = e->phase;
    set_link(e, SIG_LINK_NONE);
    if (e->sink.on_elm_link) e->sink.on_elm_link(e->sink.ctx, false);
}

void vp_engine_on_rx(vp_engine_t *e, const uint8_t *data, size_t len)
{
    size_t i;
    bool line = false;
    size_t used;

    if (!e || !data) return;
    used = e->rx_len;
    for (i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\0') continue;
        if (c == '>') e->prompt = true;
        if (c == '\r' || c == '\n' || c == '>') line = true;
        if (used < ENG_RX_CAP) e->rxbuf[used++] = c;
    }
    e->rx_len = (uint16_t)used;
    e->rxbuf[e->rx_len] = '\0';
    if (e->busy && e->cur.kind == K_MA && (line || e->rx_len >= ENG_RX_CAP)) {
        mon_consume(e, e->sink.now_ms(e->sink.ctx), false);
        e->rxbuf[e->rx_len] = '\0';
    }
}

uint32_t vp_engine_run(vp_engine_t *e)
{
    uint32_t now;
    int guard;

    if (!e) return ENG_WAIT_CAP;
    now = e->sink.now_ms(e->sink.ctx);
    if (e->phase == VP_ENGINE_PH_DOWN || e->phase == VP_ENGINE_PH_RECONNECT) return ENG_WAIT_CAP;
    if (e->link == SIG_LINK_DATA && elapsed(now, e->last_data_ms, link_hold(e))) {
        set_link(e, SIG_LINK_NO_DATA);
    }
    for (guard = 0; guard < 16; guard++) {
        eng_step_t s;

        if (e->busy) {
            if (e->cur.kind == K_MA) {
                uint32_t w = 1;
                if (!ma_poll(e, now, &w)) return w;
                continue;
            }
            if (e->prompt) {
                step_done(e, now, DONE_PROMPT);
            } else {
                uint32_t to = e->cur.timeout_ms ? e->cur.timeout_ms : resp_timeout(e);
                uint32_t el = now - e->sent_ms;
                if (el < to) return cap_wait(to - el);
                step_done(e, now, DONE_TIMEOUT);
            }
            continue;
        }
        if (e->phase == VP_ENGINE_PH_DOWN || e->phase == VP_ENGINE_PH_RECONNECT) return ENG_WAIT_CAP;
        if ((int32_t)(e->next_ms - now) > 0) return cap_wait(e->next_ms - now);
        if (e->q_n == 0) {
            plan(e, now);
            continue;
        }
        if (e->q[e->q_head].kind == K_REQ && e->req_sent &&
            !elapsed(now, e->last_req_ms, e->p->min_req_ms)) {
            return cap_wait(e->p->min_req_ms - (now - e->last_req_ms));
        }
        q_pop(e, &s);
        step_send(e, &s, now);
    }
    return 1;
}

void vp_engine_get_stats(const vp_engine_t *e, vp_engine_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!e) return;
    *out = e->st;
    out->proto = e->run_proto;
    out->phase = e->phase;
    out->link = e->link;
    out->mon_ok = e->mon_ok;
}
