// ============================================================
//  theme_jsplit.c — streaming JSON object splitter (see theme_jsplit.h)
// ============================================================

#include "theme_jsplit.h"

#include <string.h>

enum {
    S_ROOT = 0,   // before '{'
    S_KEY,        // expecting a key or '}'
    S_KEYSTR,     // inside a key string
    S_COLON,
    S_VALUE,      // expecting a value
    S_INVAL,      // scanning a value
    S_AFTER,      // expecting ',' or '}'
    S_ARR_WAIT,   // inside a split array, expecting an element, ',' or ']'
    S_ARR_ELEM,   // scanning an array element
    S_DONE,
    S_ERR,
};

enum { M_SKIP = 0, M_CAPTURE, M_SPLIT };
enum { SC_CONT = 0, SC_END_INCL, SC_END_EXCL, SC_ERR };

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool key_in(const char *const *list, const char *key)
{
    if (!list) return false;
    for (; *list; list++) {
        if (strcmp(*list, key) == 0) return true;
    }
    return false;
}

void tjs_init(tjs_t *s, const tjs_cfg_t *cfg)
{
    memset(s, 0, sizeof(*s));
    if (cfg) s->cfg = *cfg;
    s->state = S_ROOT;
    s->bom_ok = true;
}

static bool v_capturing(const tjs_t *s)
{
    return s->mode == M_CAPTURE || s->state == S_ARR_ELEM;
}

static void v_begin(tjs_t *s)
{
    s->v_started = s->v_scalar = s->v_in_str = s->v_esc = s->v_drop = s->v_overflow = false;
    s->v_depth = 0;
    s->v_pos = s->v_str_start = s->v_str_len = 0;
}

static void v_put(tjs_t *s, char c)
{
    if (!v_capturing(s) || s->v_overflow) return;
    if (!s->cfg.buf || s->v_pos + 1 >= s->cfg.cap) {
        s->v_overflow = true;
        return;
    }
    s->cfg.buf[s->v_pos++] = c;
}

static void v_put_str(tjs_t *s, char c)
{
    if (s->v_drop) return;
    s->v_str_len++;
    if (s->cfg.max_str && s->v_str_len > s->cfg.max_str) {
        // replace the whole string by ""
        if (v_capturing(s) && !s->v_overflow) s->v_pos = s->v_str_start;
        v_put(s, '"');
        v_put(s, '"');
        s->v_drop = true;
        return;
    }
    v_put(s, c);
}

static void v_str_open(tjs_t *s)
{
    s->v_in_str = true;
    s->v_esc = false;
    s->v_drop = false;
    s->v_str_len = 0;
    s->v_str_start = s->v_pos;
    v_put(s, '"');
}

static int v_scan(tjs_t *s, char c)
{
    if (!s->v_started) {
        s->v_started = true;
        if (c == '"') { v_str_open(s); return SC_CONT; }
        if (c == '{' || c == '[') { s->v_depth = 1; v_put(s, c); return SC_CONT; }
        if (c == ',' || c == '}' || c == ']' || c == ':') return SC_ERR;
        s->v_scalar = true;
        v_put(s, c);
        return SC_CONT;
    }
    if (s->v_scalar) {
        if (c == ',' || c == '}' || c == ']' || is_ws(c)) return SC_END_EXCL;
        v_put(s, c);
        return SC_CONT;
    }
    if (s->v_in_str) {
        if (s->v_esc) { s->v_esc = false; v_put_str(s, c); return SC_CONT; }
        if (c == '\\') { s->v_esc = true; v_put_str(s, c); return SC_CONT; }
        if (c == '"') {
            s->v_in_str = false;
            if (!s->v_drop) v_put(s, '"');
            s->v_drop = false;
            return s->v_depth == 0 ? SC_END_INCL : SC_CONT;
        }
        v_put_str(s, c);
        return SC_CONT;
    }
    if (is_ws(c)) return SC_CONT;
    if (c == '"') { v_str_open(s); return SC_CONT; }
    if (c == '{' || c == '[') {
        s->v_depth++;
        v_put(s, c);
        return SC_CONT;
    }
    if (c == '}' || c == ']') {
        if (s->v_depth == 0) return SC_ERR;
        s->v_depth--;
        v_put(s, c);
        return s->v_depth == 0 ? SC_END_INCL : SC_CONT;
    }
    v_put(s, c);
    return SC_CONT;
}

static bool v_emit(tjs_t *s, tjs_kind_t kind)
{
    if (!s->cfg.cb) return true;
    bool ok;
    uint32_t index = kind == TJS_ITEM ? s->index : 0;
    if (s->v_overflow || !s->cfg.buf) {
        ok = s->cfg.cb(s->cfg.ctx, kind, s->key, index, NULL, 0, true);
    } else {
        s->cfg.buf[s->v_pos] = '\0';
        ok = s->cfg.cb(s->cfg.ctx, kind, s->key, index, s->cfg.buf, s->v_pos, false);
    }
    if (!ok) s->stopped = true;
    return ok;
}

// Process one character; returns false to stop.
static bool step(tjs_t *s, char c)
{
    for (;;) {
        switch (s->state) {
        case S_ROOT:
            if (s->bom_ok && ((unsigned char)c == 0xEF || (unsigned char)c == 0xBB ||
                              (unsigned char)c == 0xBF)) {
                return true;
            }
            s->bom_ok = false;
            if (is_ws(c)) return true;
            if (c == '{') { s->state = S_KEY; return true; }
            if (c == '[' && key_in(s->cfg.split_keys, "")) {
                s->root_array = true;
                s->key[0] = '\0';
                s->mode = M_SPLIT;
                s->index = 0;
                s->state = S_ARR_WAIT;
                return true;
            }
            s->state = S_ERR;
            return false;

        case S_KEY:
            if (is_ws(c) || c == ',') return true;
            if (c == '}') { s->state = S_DONE; return true; }
            if (c == '"') {
                s->state = S_KEYSTR;
                s->key_len = 0;
                s->key_long = false;
                s->key[0] = '\0';
                s->v_esc = false;
                return true;
            }
            s->state = S_ERR;
            return false;

        case S_KEYSTR:
            if (s->v_esc) {
                s->v_esc = false;
            } else if (c == '\\') {
                s->v_esc = true;
            } else if (c == '"') {
                if (s->key_long) s->key[0] = '\0';   // never matches a configured key
                s->state = S_COLON;
                return true;
            }
            if (s->key_len + 1 < TJS_KEY_MAX) {
                s->key[s->key_len++] = c;
                s->key[s->key_len] = '\0';
            } else {
                s->key_long = true;
            }
            return true;

        case S_COLON:
            if (is_ws(c)) return true;
            if (c == ':') { s->state = S_VALUE; return true; }
            s->state = S_ERR;
            return false;

        case S_VALUE:
            if (is_ws(c)) return true;
            if (c == '[' && !s->key_long && key_in(s->cfg.split_keys, s->key)) {
                s->mode = M_SPLIT;
                s->index = 0;
                s->state = S_ARR_WAIT;
                return true;
            }
            s->mode = (!s->key_long && key_in(s->cfg.capture_keys, s->key)) ? M_CAPTURE : M_SKIP;
            v_begin(s);
            s->state = S_INVAL;
            continue;   // scan c as the first value char

        case S_INVAL: {
            int r = v_scan(s, c);
            if (r == SC_CONT) return true;
            if (r == SC_ERR) { s->state = S_ERR; return false; }
            s->state = S_AFTER;
            if (s->mode == M_CAPTURE && !v_emit(s, TJS_VALUE)) return false;
            if (r == SC_END_INCL) return true;
            continue;   // reprocess terminator
        }

        case S_AFTER:
            if (is_ws(c)) return true;
            if (c == ',') { s->state = S_KEY; return true; }
            if (c == '}') { s->state = S_DONE; return true; }
            s->state = S_ERR;
            return false;

        case S_ARR_WAIT:
            if (is_ws(c) || c == ',') return true;
            if (c == ']') { s->state = s->root_array ? S_DONE : S_AFTER; return true; }
            v_begin(s);
            s->state = S_ARR_ELEM;
            continue;

        case S_ARR_ELEM: {
            int r = v_scan(s, c);
            if (r == SC_CONT) return true;
            if (r == SC_ERR) { s->state = S_ERR; return false; }
            s->state = S_ARR_WAIT;
            bool ok = v_emit(s, TJS_ITEM);
            s->index++;
            if (!ok) return false;
            if (r == SC_END_INCL) return true;
            continue;
        }

        case S_DONE:
            if (is_ws(c) || c == '\0') return true;
            s->state = S_ERR;
            return false;

        default:
            return false;
        }
    }
}

bool tjs_feed(tjs_t *s, const char *data, size_t len)
{
    if (!s || s->stopped || s->state == S_ERR) return false;
    for (size_t i = 0; i < len; i++) {
        if (!step(s, data[i])) {
            if (s->state == S_ERR) s->error = true;
            return false;
        }
    }
    return true;
}

bool tjs_finish(const tjs_t *s)
{
    if (!s) return false;
    if (s->stopped) return true;
    return s->state == S_DONE;
}

void tjs_set_buffer(tjs_t *s, char *buf, size_t cap)
{
    if (!s) return;
    s->cfg.buf = buf;
    s->cfg.cap = cap;
}

bool tjs_run(const tjs_cfg_t *cfg, const char *data, size_t len)
{
    tjs_t s;
    tjs_init(&s, cfg);
    tjs_feed(&s, data, len);
    return tjs_finish(&s);
}
