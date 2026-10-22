#pragma once
// ============================================================
//  tr_format.h — label text formatter
//
//  Port of the editor's themePacker.ts isIntOnlyFormat() /
//  formatIntPrintf() and FabricCanvas createLabel() number path.
//  The format string comes from the theme, so it is interpreted
//  here and never handed to the libc printf family.
//
//  Supported: flags '-' '+' '0' (' ' parsed, no effect), width,
//  conversions d i u x X o c, "%%". Missing args read as 0;
//  %u of a negative prints signed decimal; '+' only for d/i.
// ============================================================

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Enough for any realistic gauge label.
#define TR_FMT_BUF  64

bool tr_fmt_is_int_only(const char *fmt);

// formatIntPrintf(fmt, args). Writes at most cap-1 bytes plus NUL, cut on a
// UTF-8 boundary. Returns the bytes written (excluding NUL).
size_t tr_fmt_int_printf(char *out, size_t cap, const char *fmt,
                         const double *args, size_t n_args);

// createLabel() without dataDisplay: fmt falls back to "%d" unless int-only;
// divisor > 1 → args [trunc(raw/d), |raw % d|], else [trunc(raw)].
// Pass divisor 0 when the element has none.
size_t tr_fmt_label(char *out, size_t cap, const char *fmt, double divisor, double raw);

#ifdef __cplusplus
}
#endif
