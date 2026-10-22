#include "tr_format.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One conversion's text before padding. Longer bodies (|v| beyond ~1e36 in
// hex) are cut; labels never get there.
#define BODY_MAX 96

// Width above this cannot fit any label buffer; parsing saturates.
#define WIDTH_MAX 4096

typedef struct {
    char  *out;
    size_t cap;
    size_t len;
    bool   full;
} sink_t;

typedef struct {
    char   s[BODY_MAX];
    size_t len;     // bytes
    size_t units;   // UTF-16 code units, what JS .length counts for padding
} body_t;

static void sink_put(sink_t *k, const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (k->len + 1 >= k->cap) {
            k->full = true;
            return;
        }
        k->out[k->len++] = p[i];
    }
}

static void sink_rep(sink_t *k, char c, size_t n)
{
    for (size_t i = 0; i < n && !k->full; i++) sink_put(k, &c, 1);
}

// NUL-terminate; when truncated, drop a trailing partial UTF-8 sequence.
static size_t sink_end(sink_t *k)
{
    size_t len = k->len;
    if (k->full && len > 0) {
        size_t j = len;
        while (j > 0 && ((unsigned char)k->out[j - 1] & 0xC0) == 0x80) j--;
        if (j > 0) {
            unsigned char lead = (unsigned char)k->out[j - 1];
            size_t need = 1;
            if ((lead & 0xE0) == 0xC0) need = 2;
            else if ((lead & 0xF0) == 0xE0) need = 3;
            else if ((lead & 0xF8) == 0xF0) need = 4;
            if (len - (j - 1) < need) len = j - 1;
        }
    }
    k->out[len] = '\0';
    return len;
}

static void body_set(body_t *b, const char *s)
{
    size_t n = strlen(s);
    if (n >= BODY_MAX) n = BODY_MAX - 1;
    memcpy(b->s, s, n);
    b->s[n] = '\0';
    b->len = n;
    b->units = n;
}

// Math.trunc(v).toString()
static void body_decimal(body_t *b, double v)
{
    if (isnan(v)) {
        body_set(b, "NaN");
        return;
    }
    if (isinf(v)) {
        body_set(b, v < 0 ? "-Infinity" : "Infinity");
        return;
    }
    v = trunc(v);
    if (v == 0) {   // also -0
        body_set(b, "0");
        return;
    }
    bool neg = v < 0;
    double a = fabs(v);
    char tmp[48];
    size_t n = 0;

    if (a < 9007199254740992.0) {   // 2^53: exact integer digits
        uint64_t u = (uint64_t)a;
        char rev[24];
        size_t r = 0;
        while (u) {
            rev[r++] = (char)('0' + (u % 10));
            u /= 10;
        }
        if (neg) tmp[n++] = '-';
        while (r) tmp[n++] = rev[--r];
        tmp[n] = '\0';
        body_set(b, tmp);
        return;
    }

    // Number::toString: shortest round-trip digits, plain up to 21 digits,
    // exponential beyond.
    char e[40] = "";
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(e, sizeof(e), "%.*e", prec - 1, a);
        if (strtod(e, NULL) == a) break;
    }
    char dig[24];
    size_t k = 0;
    const char *p = e;
    for (; *p && *p != 'e' && *p != 'E'; p++) {
        if (*p >= '0' && *p <= '9' && k < sizeof(dig) - 1) dig[k++] = *p;
    }
    int exp10 = (*p) ? atoi(p + 1) : 0;
    while (k > 1 && dig[k - 1] == '0') k--;
    int digits_n = exp10 + 1;

    if (neg) tmp[n++] = '-';
    if (digits_n <= 21) {
        for (size_t i = 0; i < k; i++) tmp[n++] = dig[i];
        for (int i = (int)k; i < digits_n; i++) tmp[n++] = '0';
        tmp[n] = '\0';
    } else {
        tmp[n++] = dig[0];
        if (k > 1) {
            tmp[n++] = '.';
            for (size_t i = 1; i < k; i++) tmp[n++] = dig[i];
        }
        snprintf(tmp + n, sizeof(tmp) - n, "e+%d", digits_n - 1);
    }
    body_set(b, tmp);
}

// ToUint32 / ToUint16
static double js_to_uint(double v, double mod)
{
    if (isnan(v) || isinf(v)) return 0.0;
    double m = fmod(trunc(v), mod);
    if (m < 0) m += mod;
    return m + 0.0;   // -0 → +0
}

// Number.prototype.toString(radix) for a non-negative value, as V8's
// DoubleToRadixCString (fraction digits up to the double's precision).
static void body_radix(body_t *b, double value, int radix)
{
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (isnan(value)) {
        body_set(b, "NaN");
        return;
    }
    if (isinf(value)) {
        body_set(b, "Infinity");
        return;
    }
    if (value == 0) {
        body_set(b, "0");
        return;
    }

    char buf[2 * BODY_MAX];
    const int mid = BODY_MAX;
    int ic = mid, fc = mid;

    double integer = floor(value);
    double fraction = value - integer;
    double delta = 0.5 * (nextafter(value, INFINITY) - value);
    double min_delta = nextafter(0.0, 1.0);
    if (!(min_delta < delta)) delta = min_delta;
    if (fraction >= delta) {
        buf[fc++] = '.';
        do {
            fraction *= radix;
            delta *= radix;
            int digit = (int)fraction;
            buf[fc++] = chars[digit];
            fraction -= digit;
            if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
                if (fraction + delta > 1) {
                    // round up, carrying back through written digits
                    for (;;) {
                        fc--;
                        if (fc == mid) {
                            integer += 1;
                            break;
                        }
                        char c = buf[fc];
                        int d = c > '9' ? (c - 'a' + 10) : (c - '0');
                        if (d + 1 < radix) {
                            buf[fc++] = chars[d + 1];
                            break;
                        }
                    }
                    break;
                }
            }
        } while (fraction >= delta && fc < (int)sizeof(buf));
    }

    // digits below the double's precision are written as zero
    while (integer / radix >= 9007199254740992.0 && ic > 1) {
        integer /= radix;
        buf[--ic] = '0';
    }
    do {
        double rem = fmod(integer, radix);
        buf[--ic] = chars[(int)rem];
        integer = (integer - rem) / radix;
    } while (integer > 0 && ic > 0);

    size_t n = (size_t)(fc - ic);
    if (n >= BODY_MAX) n = BODY_MAX - 1;
    memcpy(b->s, buf + ic, n);
    b->s[n] = '\0';
    b->len = n;
    b->units = n;
}

// String.fromCharCode(value) as UTF-8. NUL cannot be carried in a C string
// and a lone surrogate becomes U+FFFD; both still count as one unit.
static void body_char(body_t *b, double value)
{
    unsigned u = (unsigned)js_to_uint(value, 65536.0);
    size_t n = 0;
    if (u == 0) {
        n = 0;
    } else if (u < 0x80) {
        b->s[n++] = (char)u;
    } else if (u < 0x800) {
        b->s[n++] = (char)(0xC0 | (u >> 6));
        b->s[n++] = (char)(0x80 | (u & 0x3F));
    } else {
        if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
        b->s[n++] = (char)(0xE0 | (u >> 12));
        b->s[n++] = (char)(0x80 | ((u >> 6) & 0x3F));
        b->s[n++] = (char)(0x80 | (u & 0x3F));
    }
    b->s[n] = '\0';
    b->len = n;
    b->units = 1;
}

// formatOneIntConversion()
static void emit_conversion(sink_t *k, bool f_minus, bool f_plus, bool f_zero,
                            size_t width, char conv, double value)
{
    body_t b;
    switch (conv) {
    case 'x':
    case 'X':
        body_radix(&b, value < 0 ? js_to_uint(value, 4294967296.0) : value, 16);
        if (conv == 'X') {
            for (size_t i = 0; i < b.len; i++) {
                if (b.s[i] >= 'a' && b.s[i] <= 'z') b.s[i] = (char)(b.s[i] - 'a' + 'A');
            }
        }
        break;
    case 'o':
        body_radix(&b, value < 0 ? js_to_uint(value, 4294967296.0) : value, 8);
        break;
    case 'c':
        body_char(&b, value);
        break;
    default:
        body_decimal(&b, value);
        break;
    }

    bool neg = b.len > 0 && b.s[0] == '-';
    const char *digits = neg ? b.s + 1 : b.s;
    size_t digits_len = neg ? b.len - 1 : b.len;
    size_t digits_units = neg ? b.units - 1 : b.units;
    const char *sign = neg ? "-" : (f_plus && (conv == 'd' || conv == 'i')) ? "+" : "";
    size_t sign_len = strlen(sign);
    size_t used = sign_len + digits_units;
    size_t pad = width > used ? width - used : 0;

    if (f_minus) {
        sink_put(k, sign, sign_len);
        sink_put(k, digits, digits_len);
        sink_rep(k, ' ', pad);
    } else if (f_zero) {
        sink_put(k, sign, sign_len);
        sink_rep(k, '0', pad);
        sink_put(k, digits, digits_len);
    } else {
        sink_rep(k, ' ', pad);
        sink_put(k, sign, sign_len);
        sink_put(k, digits, digits_len);
    }
}

static size_t utf8_seq_len(unsigned char lead)
{
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

bool tr_fmt_is_int_only(const char *fmt)
{
    if (!fmt) return false;
    size_t len = strlen(fmt);
    for (size_t i = 0; i < len; i++) {
        if (fmt[i] != '%') continue;
        i++;
        if (i < len && fmt[i] == '%') continue;
        while (i < len && strchr("0123456789.-+ ", fmt[i])) i++;
        if (i >= len) return false;
        if (!strchr("diuxXoc", fmt[i])) return false;
    }
    return true;
}

size_t tr_fmt_int_printf(char *out, size_t cap, const char *fmt,
                         const double *args, size_t n_args)
{
    if (!out || cap == 0) return 0;
    sink_t k = { out, cap, 0, false };
    if (!fmt) fmt = "";
    size_t len = strlen(fmt);
    size_t argi = 0;

    for (size_t i = 0; i < len && !k.full; i++) {
        if (fmt[i] != '%') {
            sink_put(&k, &fmt[i], 1);
            continue;
        }
        i++;
        if (i < len && fmt[i] == '%') {
            sink_put(&k, "%", 1);
            continue;
        }
        bool f_minus = false, f_plus = false, f_zero = false;
        while (i < len && strchr("-+0 ", fmt[i])) {
            if (fmt[i] == '-') f_minus = true;
            else if (fmt[i] == '+') f_plus = true;
            else if (fmt[i] == '0') f_zero = true;
            i++;
        }
        size_t width = 0;
        while (i < len && fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (size_t)(fmt[i] - '0');
            if (width > WIDTH_MAX) width = WIDTH_MAX;
            i++;
        }
        char conv = i < len ? fmt[i] : '\0';
        double value = (args && argi < n_args) ? args[argi] : 0.0;
        argi++;
        emit_conversion(&k, f_minus, f_plus, f_zero, width, conv, value);
        // a multi-byte conversion character is one JS char: skip all of it
        if (i < len) i += utf8_seq_len((unsigned char)conv) - 1;
    }
    return sink_end(&k);
}

size_t tr_fmt_label(char *out, size_t cap, const char *fmt, double divisor, double raw)
{
    const char *f = (fmt && fmt[0] && tr_fmt_is_int_only(fmt)) ? fmt : "%d";
    double args[2];
    size_t n;
    if (divisor > 1) {
        args[0] = trunc(raw / divisor);
        args[1] = fabs(fmod(raw, divisor));
        n = 2;
    } else {
        args[0] = trunc(raw);
        n = 1;
    }
    return tr_fmt_int_printf(out, cap, f, args, n);
}
