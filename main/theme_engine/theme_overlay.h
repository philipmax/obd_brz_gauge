#pragma once
// ============================================================
//  theme_overlay.h — gaugeproj-only element fields (pure C99)
//
//  theme.bin does not carry label/bar rotation, bar direction or
//  dataDisplay. The sidecar brings them back as an "overlay" entry
//  per element; this module parses one entry and reproduces the
//  editor preview's number path bit for bit
//  (obd_theme_store@5a7484e FabricCanvas.tsx transformDataValue /
//  getElementRange / createArc / createBar / createLabel / createPointer):
//
//    v     = raw * (scale ?? 1) + (offset ?? 0)
//            then, when inputMin/inputMax/displayMin/displayMax are all
//            present and inputMax !== inputMin, mapped linearly
//            input → display
//    range = [displayMin ?? rangeMin, displayMax ?? rangeMax]
//    label = valueMap[String(Math.trunc(v))]
//            ?? (precision !== undefined ? v.toFixed(precision)
//            : divisor > 1 ? formatIntPrintf(fmt, [trunc(v/d), |v % d|])
//            : formatIntPrintf(fmt, [trunc(v)]))
//            then + " " + unit when unit is a non-empty string
//
//  Device rule on top: a bound label whose raw value is invalid (NaN)
//  shows THEME_INVALID_TEXT without unit.
//  Non-number dataDisplay fields are ignored (the editor only writes
//  numbers there); JSON null follows JS (`??` → default, `!== undefined`
//  → present with value 0).
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THEME_INVALID_TEXT  "--"

// Number.prototype.toFixed accepts 0..100.
#define THEME_OV_MAX_PRECISION  100

typedef enum {
    THEME_OV_ABSENT = 0,
    THEME_OV_NULL,          // JSON null
    THEME_OV_NUM,
} theme_ov_tag_t;

typedef struct {
    uint8_t tag;            // theme_ov_tag_t
    double  v;              // 0 unless THEME_OV_NUM
} theme_ov_num_t;

typedef struct {
    const char *key;
    const char *text;
} theme_ov_map_t;

typedef struct {
    uint8_t  kind;          // tr_kind_t of the overlay entry (0 = unknown)
    uint8_t  has_rotation;
    uint8_t  vertical;      // bar.direction === 'vertical'
    uint8_t  has_dd;        // dataDisplay object present
    double   rotation;      // element.rotation || 0

    theme_ov_num_t scale, offset;
    theme_ov_num_t in_min, in_max, disp_min, disp_max;
    uint8_t  has_precision;
    uint8_t  precision;     // clamped to 0..THEME_OV_MAX_PRECISION
    const char *unit;       // NULL or non-empty
    uint16_t n_map;
    const theme_ov_map_t *map;   // JSON order; lookup takes the last duplicate
} theme_overlay_t;

typedef struct {
    void *(*alloc_fn)(size_t size, void *ctx);
    void (*free_fn)(void *ptr, void *ctx);
    void *ctx;
} theme_ov_alloc_t;

// Parse one overlay entry {"kind","rotation","direction","dataDisplay"}.
// *out is a single allocation (free with theme_overlay_free). Returns false
// on allocation failure or when `entry` is not an object.
bool theme_overlay_parse(const cJSON *entry, const theme_ov_alloc_t *alloc,
                         theme_overlay_t **out);
void theme_overlay_free(theme_overlay_t *ov, const theme_ov_alloc_t *alloc);

// transformDataValue(); ov may be NULL (identity).
double theme_ov_transform(const theme_overlay_t *ov, double raw);

// getElementRange().
void theme_ov_range(const theme_overlay_t *ov, double range_min, double range_max,
                    double *lo, double *hi);

// Math.max(0, Math.min(1, (v - lo) / (hi - lo))) with JS NaN propagation.
double theme_ov_progress(double v, double lo, double hi);

// Pointer angle: clamp → linear map; range <= 0 → angle_min (NaN v → NaN).
double theme_ov_pointer_angle(double v, double lo, double hi,
                              double angle_min, double angle_max);

// Number.prototype.toFixed(f). Exact (round half up on |x|, as the spec's
// "pick the larger n"). Returns bytes written (excluding NUL).
size_t theme_js_to_fixed(double x, int f, char *out, size_t cap);

// Number::toString(10) (shortest round-trip digits, JS exponent rules).
size_t theme_js_num_to_str(double x, char *out, size_t cap);

// createLabel() for a bound label. fmt may be NULL, divisor 0 when absent.
// raw NaN → THEME_INVALID_TEXT.
size_t theme_ov_label_text(const theme_overlay_t *ov, const char *fmt, double divisor,
                           double raw, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
