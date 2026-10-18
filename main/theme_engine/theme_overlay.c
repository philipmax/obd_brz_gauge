// ============================================================
//  theme_overlay.c — gaugeproj-only element fields (see theme_overlay.h)
// ============================================================

#include "theme_overlay.h"
#include "rules/tr_parse.h"
#include "rules/tr_format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------- allocation ----------------

static void *ov_alloc(const theme_ov_alloc_t *a, size_t size)
{
    void *p = (a && a->alloc_fn) ? a->alloc_fn(size, a->ctx) : malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void theme_overlay_free(theme_overlay_t *ov, const theme_ov_alloc_t *alloc)
{
    if (!ov) return;
    if (alloc && alloc->alloc_fn) {
        if (alloc->free_fn) alloc->free_fn(ov, alloc->ctx);
    } else {
        free(ov);
    }
}

// ---------------- JS number helpers ----------------

static double js_min(double a, double b)
{
    if (isnan(a) || isnan(b)) return NAN;
    if (a == b) return signbit(a) ? a : b;
    return a < b ? a : b;
}

static double js_max(double a, double b)
{
    if (isnan(a) || isnan(b)) return NAN;
    if (a == b) return signbit(a) ? b : a;
    return a > b ? a : b;
}

// ---------------- parse ----------------

// JSON.parse semantics: the last duplicate key wins.
static const cJSON *jget(const cJSON *obj, const char *key)
{
    const cJSON *hit = NULL;
    if (!cJSON_IsObject(obj)) return NULL;
    for (const cJSON *c = obj->child; c; c = c->next) {
        if (c->string && strcmp(c->string, key) == 0) hit = c;
    }
    return hit;
}

static void ov_num(const cJSON *obj, const char *key, theme_ov_num_t *out)
{
    const cJSON *it = jget(obj, key);
    out->tag = THEME_OV_ABSENT;
    out->v = 0;
    if (!it) return;
    if (cJSON_IsNull(it)) {
        out->tag = THEME_OV_NULL;
    } else if (cJSON_IsNumber(it)) {
        out->tag = THEME_OV_NUM;
        out->v = it->valuedouble;
    }
}

// valueMap value → text (String() of the JSON value); NULL = not representable.
static const char *ov_map_text(const cJSON *v, char *tmp, size_t cap)
{
    if (cJSON_IsString(v)) return v->valuestring;
    if (cJSON_IsNumber(v)) {
        theme_js_num_to_str(v->valuedouble, tmp, cap);
        return tmp;
    }
    if (cJSON_IsTrue(v)) return "true";
    if (cJSON_IsFalse(v)) return "false";
    if (cJSON_IsNull(v)) return "null";
    return NULL;
}

bool theme_overlay_parse(const cJSON *entry, const theme_ov_alloc_t *alloc,
                         theme_overlay_t **out)
{
    if (!out) return false;
    *out = NULL;
    if (!cJSON_IsObject(entry)) return false;

    const cJSON *dd = jget(entry, "dataDisplay");
    if (!cJSON_IsObject(dd)) dd = NULL;
    const cJSON *unit = dd ? jget(dd, "unit") : NULL;
    const cJSON *vmap = dd ? jget(dd, "valueMap") : NULL;
    if (!cJSON_IsObject(vmap)) vmap = NULL;

    // pass 1: size
    char tmp[40];
    size_t pool = 0;
    uint16_t n_map = 0;
    if (cJSON_IsString(unit) && unit->valuestring[0]) pool += strlen(unit->valuestring) + 1;
    if (vmap) {
        for (const cJSON *m = vmap->child; m && n_map < UINT16_MAX; m = m->next) {
            const char *t = ov_map_text(m, tmp, sizeof(tmp));
            if (!t || !m->string) continue;
            pool += strlen(m->string) + 1 + strlen(t) + 1;
            n_map++;
        }
    }
    size_t total = sizeof(theme_overlay_t) + (size_t)n_map * sizeof(theme_ov_map_t) + pool;
    uint8_t *mem = ov_alloc(alloc, total);
    if (!mem) return false;

    theme_overlay_t *ov = (theme_overlay_t *)mem;
    theme_ov_map_t *map = (theme_ov_map_t *)(mem + sizeof(theme_overlay_t));
    char *sp = (char *)(map + n_map);

    const cJSON *kind = jget(entry, "kind");
    ov->kind = (uint8_t)tr_kind_from_str(cJSON_IsString(kind) ? kind->valuestring : NULL);

    const cJSON *rot = jget(entry, "rotation");
    if (cJSON_IsNumber(rot)) {
        ov->has_rotation = 1;
        ov->rotation = rot->valuedouble;   // `rotation || 0`: 0 stays 0
    }
    const cJSON *dir = jget(entry, "direction");
    ov->vertical = (cJSON_IsString(dir) && strcmp(dir->valuestring, "vertical") == 0) ? 1 : 0;

    if (dd) {
        ov->has_dd = 1;
        ov_num(dd, "scale", &ov->scale);
        ov_num(dd, "offset", &ov->offset);
        ov_num(dd, "inputMin", &ov->in_min);
        ov_num(dd, "inputMax", &ov->in_max);
        ov_num(dd, "displayMin", &ov->disp_min);
        ov_num(dd, "displayMax", &ov->disp_max);

        const cJSON *prec = jget(dd, "precision");
        if (prec) {
            // toFixed(ToIntegerOrInfinity(p)); null → 0. Out-of-range values
            // throw in the editor; the device clamps.
            double p = 0;
            bool ok = true;
            if (cJSON_IsNumber(prec)) p = prec->valuedouble;
            else if (cJSON_IsTrue(prec)) p = 1;
            else if (cJSON_IsNull(prec) || cJSON_IsFalse(prec)) p = 0;
            else ok = false;
            if (ok) {
                p = isnan(p) ? 0 : trunc(p);
                if (p < 0) p = 0;
                if (p > THEME_OV_MAX_PRECISION) p = THEME_OV_MAX_PRECISION;
                ov->has_precision = 1;
                ov->precision = (uint8_t)p;
            }
        }

        if (cJSON_IsString(unit) && unit->valuestring[0]) {
            size_t l = strlen(unit->valuestring) + 1;
            memcpy(sp, unit->valuestring, l);
            ov->unit = sp;
            sp += l;
        }
        uint16_t i = 0;
        if (vmap) {
            for (const cJSON *m = vmap->child; m && i < n_map; m = m->next) {
                const char *t = ov_map_text(m, tmp, sizeof(tmp));
                if (!t || !m->string) continue;
                size_t lk = strlen(m->string) + 1, lt = strlen(t) + 1;
                memcpy(sp, m->string, lk);
                map[i].key = sp;
                sp += lk;
                memcpy(sp, t, lt);
                map[i].text = sp;
                sp += lt;
                i++;
            }
        }
        ov->n_map = i;
        ov->map = i ? map : NULL;
    }

    *out = ov;
    return true;
}

// ---------------- value path ----------------

double theme_ov_transform(const theme_overlay_t *ov, double raw)
{
    if (!ov || !ov->has_dd) return raw * 1.0 + 0.0;
    double scale = ov->scale.tag == THEME_OV_NUM ? ov->scale.v : 1.0;
    double offset = ov->offset.tag == THEME_OV_NUM ? ov->offset.v : 0.0;
    double value = raw * scale + offset;
    if (ov->in_min.tag != THEME_OV_ABSENT && ov->in_max.tag != THEME_OV_ABSENT &&
        ov->disp_min.tag != THEME_OV_ABSENT && ov->disp_max.tag != THEME_OV_ABSENT) {
        // `inputMax !== inputMin`: null === null, null !== 0
        bool same;
        if (ov->in_min.tag != ov->in_max.tag) same = false;
        else if (ov->in_min.tag == THEME_OV_NULL) same = true;
        else same = ov->in_max.v == ov->in_min.v;
        if (!same) {
            double progress = (value - ov->in_min.v) / (ov->in_max.v - ov->in_min.v);
            value = ov->disp_min.v + progress * (ov->disp_max.v - ov->disp_min.v);
        }
    }
    return value;
}

void theme_ov_range(const theme_overlay_t *ov, double range_min, double range_max,
                    double *lo, double *hi)
{
    double a = range_min, b = range_max;
    if (ov && ov->has_dd) {
        if (ov->disp_min.tag == THEME_OV_NUM) a = ov->disp_min.v;
        if (ov->disp_max.tag == THEME_OV_NUM) b = ov->disp_max.v;
    }
    if (lo) *lo = a;
    if (hi) *hi = b;
}

double theme_ov_progress(double v, double lo, double hi)
{
    return js_max(0.0, js_min(1.0, (v - lo) / (hi - lo)));
}

double theme_ov_pointer_angle(double v, double lo, double hi,
                              double angle_min, double angle_max)
{
    double clamped = js_max(lo, js_min(hi, v));
    double value_range = hi - lo;
    double angle_range = angle_max - angle_min;
    return value_range > 0 ? angle_min + ((clamped - lo) * angle_range) / value_range
                           : angle_min;
}

// ---------------- toFixed (exact) ----------------

#define BN_LIMBS 16   // 512 bits: mantissa (53) * 10^100 (333) * 2^17 fits

typedef struct {
    uint32_t d[BN_LIMBS];
    int n;            // used limbs
} bn_t;

static void bn_set_u64(bn_t *b, uint64_t v)
{
    memset(b, 0, sizeof(*b));
    while (v) {
        b->d[b->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static bool bn_mul_small(bn_t *b, uint32_t m)
{
    uint64_t carry = 0;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->d[i] * m + carry;
        b->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        if (b->n >= BN_LIMBS) return false;
        b->d[b->n++] = (uint32_t)carry;
    }
    return true;
}

static int bn_bitlen(const bn_t *b)
{
    if (b->n == 0) return 0;
    uint32_t top = b->d[b->n - 1];
    int bits = 0;
    while (top) { bits++; top >>= 1; }
    return (b->n - 1) * 32 + bits;
}

static bool bn_add_pow2(bn_t *b, int k)
{
    int limb = k / 32;
    if (limb >= BN_LIMBS) return false;
    while (b->n <= limb) b->d[b->n++] = 0;
    uint64_t carry = (uint64_t)1u << (k % 32);
    for (int i = limb; i < BN_LIMBS && carry; i++) {
        if (i >= b->n) b->d[b->n++] = 0;
        uint64_t t = (uint64_t)b->d[i] + carry;
        b->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    return carry == 0;
}

static bool bn_shl(bn_t *b, int k)
{
    if (b->n == 0 || k == 0) return true;
    if (bn_bitlen(b) + k > BN_LIMBS * 32) return false;
    int limbs = k / 32, bits = k % 32;
    bn_t r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < b->n; i++) {
        uint64_t v = (uint64_t)b->d[i] << bits;
        r.d[i + limbs] |= (uint32_t)v;
        if (i + limbs + 1 < BN_LIMBS) r.d[i + limbs + 1] |= (uint32_t)(v >> 32);
    }
    r.n = BN_LIMBS;
    while (r.n > 0 && r.d[r.n - 1] == 0) r.n--;
    *b = r;
    return true;
}

static void bn_shr(bn_t *b, int k)
{
    int limbs = k / 32, bits = k % 32;
    if (limbs >= b->n) { memset(b, 0, sizeof(*b)); return; }
    bn_t r;
    memset(&r, 0, sizeof(r));
    for (int i = limbs; i < b->n; i++) {
        uint64_t v = b->d[i];
        if (i + 1 < b->n) v |= (uint64_t)b->d[i + 1] << 32;
        r.d[i - limbs] = (uint32_t)(v >> bits);
    }
    r.n = b->n - limbs;
    while (r.n > 0 && r.d[r.n - 1] == 0) r.n--;
    *b = r;
}

static uint32_t bn_divmod_small(bn_t *b, uint32_t m)
{
    uint64_t rem = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | b->d[i];
        b->d[i] = (uint32_t)(cur / m);
        rem = cur % m;
    }
    while (b->n > 0 && b->d[b->n - 1] == 0) b->n--;
    return (uint32_t)rem;
}

// Decimal digits of b (destroys b). "0" for zero. Returns length.
static size_t bn_to_dec(bn_t *b, char *buf, size_t cap)
{
    char rev[BN_LIMBS * 10 + 2];
    size_t n = 0;
    if (b->n == 0) rev[n++] = '0';
    while (b->n > 0 && n + 9 < sizeof(rev)) {
        uint32_t chunk = bn_divmod_small(b, 1000000000u);
        for (int i = 0; i < 9; i++) {
            rev[n++] = (char)('0' + chunk % 10);
            chunk /= 10;
            if (b->n == 0 && chunk == 0) break;
        }
    }
    size_t len = 0;
    for (size_t i = 0; i < n && len + 1 < cap; i++) buf[len++] = rev[n - 1 - i];
    if (cap) buf[len] = '\0';
    return len;
}

static size_t ov_copy(char *out, size_t cap, const char *s)
{
    if (!out || cap == 0) return 0;
    size_t l = strlen(s);
    if (l >= cap) l = cap - 1;
    memcpy(out, s, l);
    out[l] = '\0';
    return l;
}

size_t theme_js_to_fixed(double x, int f, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    if (f < 0) f = 0;
    if (f > THEME_OV_MAX_PRECISION) f = THEME_OV_MAX_PRECISION;
    if (isnan(x)) return ov_copy(out, cap, "NaN");
    if (!(fabs(x) < 1e21)) return theme_js_num_to_str(x, out, cap);

    bool neg = x < 0;            // -0 is not negative here
    double ax = fabs(x);

    // ax = mant * 2^e, mant < 2^53
    int ex = 0;
    double fr = frexp(ax, &ex);
    uint64_t mant = (uint64_t)ldexp(fr, 53);
    int e = ex - 53;

    bn_t n;
    bn_set_u64(&n, mant);
    // * 10^f
    for (int k = f; k > 0; ) {
        int step = k > 9 ? 9 : k;
        uint32_t m = 1;
        for (int i = 0; i < step; i++) m *= 10u;
        if (!bn_mul_small(&n, m)) return ov_copy(out, cap, "NaN");
        k -= step;
    }
    if (e >= 0) {
        if (!bn_shl(&n, e)) return ov_copy(out, cap, "NaN");
    } else {
        int d = -e;
        if (d - 1 >= bn_bitlen(&n)) {
            memset(&n, 0, sizeof(n));    // < 1/2 → 0
        } else {
            if (!bn_add_pow2(&n, d - 1)) return ov_copy(out, cap, "NaN");
            bn_shr(&n, d);               // floor((P + 2^(d-1)) / 2^d): ties up
        }
    }

    char digits[BN_LIMBS * 10 + 2];
    size_t k = bn_to_dec(&n, digits, sizeof(digits));

    char buf[BN_LIMBS * 10 + 8];
    size_t len = 0;
    if (neg) buf[len++] = '-';
    if (f == 0) {
        memcpy(buf + len, digits, k);
        len += k;
    } else {
        size_t fz = (size_t)f;
        if (k <= fz) {
            // "0." + zeros + digits
            buf[len++] = '0';
            buf[len++] = '.';
            for (size_t i = 0; i < fz - k; i++) buf[len++] = '0';
            memcpy(buf + len, digits, k);
            len += k;
        } else {
            memcpy(buf + len, digits, k - fz);
            len += k - fz;
            buf[len++] = '.';
            memcpy(buf + len, digits + (k - fz), fz);
            len += fz;
        }
    }
    buf[len] = '\0';
    return ov_copy(out, cap, buf);
}

size_t theme_js_num_to_str(double x, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    if (isnan(x)) return ov_copy(out, cap, "NaN");
    if (x == 0) return ov_copy(out, cap, "0");
    if (isinf(x)) return ov_copy(out, cap, x < 0 ? "-Infinity" : "Infinity");

    char buf[64];
    size_t len = 0;
    if (x < 0) {
        buf[len++] = '-';
        x = -x;
    }
    // Shortest round-trip digits (integers too: 2^60 prints as
    // "1152921504606847000"): the first precision whose correctly
    // rounded %e form reads back as x (closest candidate of that length).
    char e[40];
    for (int p = 1; p <= 17; p++) {
        snprintf(e, sizeof(e), "%.*e", p - 1, x);
        if (strtod(e, NULL) == x) break;
    }
    char digs[24];
    size_t k = 0;
    const char *c = e;
    for (; *c && *c != 'e' && *c != 'E'; c++) {
        if (*c >= '0' && *c <= '9' && k + 1 < sizeof(digs)) digs[k++] = *c;
    }
    int exp10 = (*c == 'e' || *c == 'E') ? atoi(c + 1) : 0;
    while (k > 1 && digs[k - 1] == '0') k--;
    digs[k] = '\0';
    int n = exp10 + 1;
    int kk = (int)k;

    char *o = buf + len;
    size_t room = sizeof(buf) - len;
    if (kk <= n && n <= 21) {
        size_t w = 0;                       // n <= 21 always fits
        for (int i = 0; i < kk; i++) o[w++] = digs[i];
        for (int i = 0; i < n - kk; i++) o[w++] = '0';
        o[w] = '\0';
    } else if (0 < n && n <= 21) {
        snprintf(o, room, "%.*s.%s", n, digs, digs + n);
    } else if (-6 < n && n <= 0) {
        snprintf(o, room, "0.%.*s%s", -n, "000000", digs);
    } else {
        int ee = n - 1;
        if (kk == 1) snprintf(o, room, "%se%c%d", digs, ee < 0 ? '-' : '+', ee < 0 ? -ee : ee);
        else snprintf(o, room, "%c.%se%c%d", digs[0], digs + 1, ee < 0 ? '-' : '+',
                      ee < 0 ? -ee : ee);
    }
    return ov_copy(out, cap, buf);
}

// ---------------- label ----------------

static const char *ov_map_lookup(const theme_overlay_t *ov, const char *key)
{
    if (!ov || !ov->map) return NULL;
    for (int i = (int)ov->n_map - 1; i >= 0; i--) {   // JSON.parse: last duplicate wins
        if (strcmp(ov->map[i].key, key) == 0) return ov->map[i].text;
    }
    return NULL;
}

size_t theme_ov_label_text(const theme_overlay_t *ov, const char *fmt, double divisor,
                           double raw, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    if (isnan(raw)) return ov_copy(out, cap, THEME_INVALID_TEXT);

    double v = theme_ov_transform(ov, raw);
    char text[160];
    char key[48];
    theme_js_num_to_str(trunc(v), key, sizeof(key));
    const char *mapped = ov_map_lookup(ov, key);
    if (mapped) {
        ov_copy(text, sizeof(text), mapped);
    } else if (ov && ov->has_dd && ov->has_precision) {
        theme_js_to_fixed(v, ov->precision, text, sizeof(text));
    } else {
        tr_fmt_label(text, sizeof(text), fmt, divisor, v);
    }
    size_t len = strlen(text);
    if (ov && ov->unit && len + 1 < sizeof(text)) {
        snprintf(text + len, sizeof(text) - len, " %s", ov->unit);
    }
    return ov_copy(out, cap, text);
}
