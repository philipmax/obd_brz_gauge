#pragma once
// ============================================================
//  theme_sidecar_core.h — sidecar reduce + pairing (pure C99)
//
//  Input: a full editor <id>.gaugeproj.json (multi-MB, streamed) or an
//  already reduced "gauge-sidecar/1" document. Output: the reduced
//  sidecar (contracts/theme-editor-contract.md B2):
//
//  {"format":"gauge-sidecar/1","theme_id":"..","project_version":"..",
//   "source":{"kind":"gaugeproj","size":N,"sha256":".."},
//   "asset_files":{"<asset name>":"<fileName>"},
//   "pages":[{"id":"..","element_ids":[..],"elements":{"<id>":"<kind>"},
//             "overlay":{"<id>":{"kind":..,"rotation":..,"direction":..,
//                                "dataDisplay":{..}}},
//             "rules":[GaugeRule verbatim]}]}
//
//  - overlay entries only for label rotation (!= 0), bar rotation
//    (!= 0) / direction, and dataDisplay of arc/bar/label/pointer;
//  - rules beyond TR_MAX_RULES per page are dropped (counted);
//  - asset_files (optional) maps asset names to gaugeproj fileName,
//    used by vid_/video_ elements to switch videos with set.asset;
//  - total output <= TSC_MAX_BYTES.
//  No ESP-IDF / LVGL dependency (test/host/theme).
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rules/tr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TSC_FORMAT          "gauge-sidecar/1"
#define TSC_MAX_BYTES       (64u * 1024u)   // stored sidecar
#define TSC_MAX_PAGES       32u
#define TSC_MAX_ELEMENTS    128u            // element ids kept per page
#define TSC_MAX_STR         4096u           // longer strings are dropped while streaming
#define TSC_CAPTURE_MAX     (64u * 1024u)   // one gaugeproj page, compact
#define TSC_ITEM_MAX        (16u * 1024u)   // one element / rule / overlay map
#define TSC_ID_MAX          128u            // longer element ids are ignored

typedef struct {
    void *(*alloc_fn)(size_t size, void *ctx);
    void (*free_fn)(void *ptr, void *ctx);
    void *ctx;
} tsc_alloc_t;

typedef enum {
    TSC_OK = 0,
    TSC_ERR_JSON,        // not a JSON object / truncated
    TSC_ERR_FORMAT,      // "format" present but not gauge-sidecar/1
    TSC_ERR_THEME_ID,    // missing or invalid theme id
    TSC_ERR_TOO_BIG,     // reduced output > TSC_MAX_BYTES
    TSC_ERR_NOMEM,
} tsc_err_t;

typedef struct {
    char     theme_id[72];
    char     version[32];
    bool     reduced_input;        // input already was gauge-sidecar/1
    uint16_t pages;                // pages kept
    uint16_t pages_over_limit;     // beyond TSC_MAX_PAGES
    uint16_t pages_too_big;        // capture or output overflow
    uint16_t pages_invalid;        // no id / unparsable
    uint16_t rules;                // rules kept
    uint16_t rules_truncated;      // beyond TR_MAX_RULES
    uint16_t elements;
    uint16_t elements_truncated;
    uint16_t overlays;
    uint16_t asset_files;
} tsc_stats_t;

typedef struct tsc_builder tsc_builder_t;

tsc_builder_t *tsc_builder_new(const tsc_alloc_t *alloc);

// Feed the next chunk of the input document.
bool tsc_builder_feed(tsc_builder_t *b, const char *data, size_t len);

// Finish: *out (from the allocator, NUL-terminated) holds the reduced
// sidecar. src_size / sha256_hex describe the input (gaugeproj source).
tsc_err_t tsc_builder_finish(tsc_builder_t *b, uint64_t src_size, const char *sha256_hex,
                             char **out, size_t *out_len, tsc_stats_t *st);

void tsc_builder_free(tsc_builder_t *b);

// ^[A-Za-z][A-Za-z0-9_-]{0,63}$ (editor rule; also keeps paths safe).
bool tsc_valid_theme_id(const char *id);

// ---------------- reading a stored sidecar ----------------

// Page-level pieces of one reduced page (pointers into `scratch`).
typedef struct {
    char id[TSC_ID_MAX];
    const char *element_ids;  size_t element_ids_len;   // JSON array text
    const char *elements;     size_t elements_len;      // JSON object text
    const char *overlay;      size_t overlay_len;       // JSON object text
    const char *rules;        size_t rules_len;         // JSON array text
} tsc_page_t;

// Find page `page_id` (NULL = by index) in a stored sidecar. The page text
// and its pieces are copied into scratch (2 * TSC_MAX_BYTES always fits).
// Returns false when the page is absent.
bool tsc_find_page(const char *json, size_t len, const char *page_id, uint32_t index,
                   char *scratch, size_t scratch_cap, tsc_page_t *page);

// Top-level header fields of a stored sidecar (theme id, version).
bool tsc_read_header(const char *json, size_t len, char *theme_id, size_t id_cap,
                     char *version, size_t ver_cap, uint16_t *n_pages);

// asset_files lookup (asset name → fileName); scratch >= TSC_ITEM_MAX.
bool tsc_asset_file(const char *json, size_t len, const char *asset,
                    char *scratch, size_t scratch_cap, char *out, size_t cap);

// ---------------- pairing ----------------

typedef bool (*tsc_asset_exists_fn)(const char *name, void *ctx);

typedef struct {
    uint16_t sc_elements;        // entries in the sidecar elements map
    uint16_t matched;            // same id and kind in theme.bin
    uint16_t missing;            // id absent from theme.bin
    uint16_t kind_mismatch;
    bool     in_sync;            // element_ids == theme.bin element order
    uint16_t rules;
    uint16_t actions;
    uint16_t actions_no_target;  // target absent or kind mismatch → dropped
    uint16_t actions_no_asset;   // set.asset names an unknown asset → ignored
    uint16_t actions_truncated;  // beyond TR_MAX_ACTIONS per rule
    uint16_t conds_truncated;    // beyond TR_MAX_CONDS per rule
    uint16_t overlays;
    uint16_t overlays_unmatched; // overlay entry for an unmatched element
} tsc_pair_t;

// Pair one sidecar page with the theme.bin layout elements of the same page.
void tsc_pair_page(const tsc_page_t *page, const tr_element_t *bin, uint16_t n_bin,
                   tsc_asset_exists_fn asset_exists, void *ctx, tsc_pair_t *out);

#ifdef __cplusplus
}
#endif
