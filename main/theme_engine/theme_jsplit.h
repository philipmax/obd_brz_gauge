#pragma once
// ============================================================
//  theme_jsplit.h — streaming JSON object splitter (pure C99)
//
//  Feeds a JSON object byte by byte and hands selected top-level
//  values to a callback as compact JSON text (whitespace outside
//  strings removed), without ever building a tree of the whole
//  document:
//    - capture_keys: the whole value is captured;
//    - split_keys:   an array value is split, every element captured
//                    on its own (a non-array value falls back to
//                    capture_keys, else is skipped);
//    - every other key is scanned and skipped without buffering.
//  Strings longer than max_str bytes are replaced by "" inside
//  captures (base64 dataUrl, compiled fonts). A capture that does not
//  fit the buffer is reported with overflow = true and len = 0.
//
//  A root array is accepted when split_keys contains "" (each element
//  is reported as TJS_ITEM with key "").
//
//  Used for multi-MB gaugeproj files (SD import), the stored sidecar
//  and theme.bin page layouts (one element parsed at a time keeps
//  cJSON trees small).
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TJS_KEY_MAX 32

typedef enum {
    TJS_VALUE = 0,   // capture_keys value
    TJS_ITEM,        // element `index` of a split_keys array
} tjs_kind_t;

// Return false to stop scanning (tjs_feed then returns false with
// tjs_t.stopped set; not an error).
typedef bool (*tjs_cb_fn)(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                          const char *json, size_t len, bool overflow);

typedef struct {
    const char *const *capture_keys;   // NULL-terminated, may be NULL
    const char *const *split_keys;     // NULL-terminated, may be NULL
    size_t max_str;                    // 0 = keep every string
    char *buf;                         // capture buffer
    size_t cap;
    tjs_cb_fn cb;
    void *ctx;
} tjs_cfg_t;

typedef struct {
    tjs_cfg_t cfg;
    uint8_t state;
    uint8_t mode;            // what the current value does
    bool stopped;
    bool error;
    bool bom_ok;
    bool root_array;
    char key[TJS_KEY_MAX];
    uint8_t key_len;
    bool key_long;
    uint32_t index;          // element index inside a split array
    // value scanner
    bool v_started, v_scalar, v_in_str, v_esc, v_drop, v_overflow;
    uint32_t v_depth;
    size_t v_pos, v_str_start, v_str_len;
} tjs_t;

void tjs_init(tjs_t *s, const tjs_cfg_t *cfg);

// false on a structural error or when the callback stopped the scan.
bool tjs_feed(tjs_t *s, const char *data, size_t len);

// true when the root object was closed (or the callback stopped the scan).
bool tjs_finish(const tjs_t *s);

// Change the capture buffer for the next capture (callable from the callback,
// e.g. to keep a captured value in place and capture the next one after it).
void tjs_set_buffer(tjs_t *s, char *buf, size_t cap);

// One-shot helper over a complete buffer.
bool tjs_run(const tjs_cfg_t *cfg, const char *data, size_t len);

#ifdef __cplusplus
}
#endif
