#pragma once
// ============================================================
//  theme_layout.h — theme.bin page layout parser (pure C99)
//
//  layout = {"page_id","background_color","elements":[...]} (compact JSON,
//  <= 64 KB, editor themePacker.ts:254-260). The elements array is split
//  with the streaming splitter and each element is parsed on its own, so
//  only one small cJSON tree exists at a time.
//
//  Output is plain data in editor units (doubles, snake_case fields of
//  contracts/theme-editor-contract.md A5); pixel rounding R(v) happens
//  where LVGL objects are built. Shared by the page builder and the
//  sidecar pairing report. No ESP-IDF / LVGL dependency (test/host/theme).
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rules/tr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TL_MAX_ELEMENTS   128             // per page; later elements are skipped
#define TL_MAX_BINDINGS   32              // editor limit; later data_source fields are dropped
#define TL_ITEM_MAX       (16u * 1024u)   // scratch for one element (compact JSON)
#define TL_PX_MAX         8191            // |R(v)| clamp (LVGL 8 coordinate range)

typedef enum {
    TL_ALIGN_LEFT = 0,
    TL_ALIGN_CENTER,
    TL_ALIGN_RIGHT,
} tl_align_t;

typedef struct {
    void *(*alloc_fn)(size_t size, void *ctx);
    void (*free_fn)(void *ptr, void *ctx);
    void *ctx;
} tl_alloc_t;

// manifest "colors" (0xRRGGBB) used when a layout color is missing / invalid
typedef struct {
    uint32_t bg;              // page background
    uint32_t arc_track;       // arc/bar bg_color
    uint32_t arc_indicator;   // arc/bar color
    uint32_t text_primary;    // label color
} tl_defaults_t;

typedef struct {
    const char *id;           // never NULL ("" when absent)
    uint8_t  kind;            // tr_kind_t, never TR_KIND_UNKNOWN
    uint8_t  align;           // tl_align_t (label)
    uint8_t  video;           // image whose id starts with vid_ / video_
    double   x, y, width, height;
    double   start_angle, end_angle, rotation;
    double   range_min, range_max;
    double   angle_min, angle_max, pivot_x, pivot_y;   // pointer
    double   line_width, radius, divisor, font_size;
    double   opacity;         // image/video layer opacity, 0..100; defaults to 100
    uint32_t color, bg_color; // 0xRRGGBB, defaults applied
    const char *data_source;  // NULL: absent or beyond TL_MAX_BINDINGS
    const char *format;       // NULL when absent
    const char *text;         // NULL when absent
    const char *asset;        // NULL when absent / empty
    const char *font_asset;   // NULL when absent / empty
    uint8_t video_value;      // OBV1 position follows data_source instead of time
    double video_step;        // 0 = every frame; positive = input quantization
    double bg_opacity;        // arc/bar track only, 0..100; omitted = 100
} tl_elem_t;

typedef struct {
    uint32_t bg_color;        // background_color, else tl_defaults_t.bg
    uint16_t n;
    tl_elem_t *el;            // [n], array order = z order (later on top)
    uint16_t unknown_types;   // skipped: unknown "type"
    uint16_t invalid;         // skipped: not an object / too big / unparsable
    uint16_t over_limit;      // skipped: beyond TL_MAX_ELEMENTS
    uint16_t bindings_dropped;// data_source beyond TL_MAX_BINDINGS
    char first_unknown[24];   // first unknown type string (diagnostics)
    uint16_t cap;             // allocated el[] slots
} tl_layout_t;

// Parse one layout. scratch (>= TL_ITEM_MAX recommended) holds one element
// at a time. Returns NULL when the root is not a JSON object or on NOMEM.
tl_layout_t *tl_parse(const char *json, size_t len, const tl_defaults_t *def,
                      const tl_alloc_t *alloc, char *scratch, size_t scratch_cap);

void tl_free(tl_layout_t *l, const tl_alloc_t *alloc);

// R(v) = floor(v + 0.5) in double (JS Math.round; R(-0.5) = 0, R(-120) = -120).
double tl_round(double v);

// R(v) clamped to [-TL_PX_MAX, TL_PX_MAX]; NaN → 0.
int32_t tl_px(double v);

// vid_ / video_ prefix; *key (optional) = id without the prefix.
bool tl_is_video_id(const char *id, const char **key);

#ifdef __cplusplus
}
#endif
