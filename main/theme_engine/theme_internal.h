#pragma once
// ============================================================
//  theme_internal.h — theme engine private state (not a public API)
//
//  Files: theme_loader.c (manifest, tables, SD reads, fonts, lifecycle),
//  theme_page.c (page build, bindings, rules), theme_sidecar.c
//  (sidecar install / pairing report), theme_img_cache.c.
//
//  Memory: the whole context and everything it owns lives in PSRAM
//  (heap_caps_*(MALLOC_CAP_SPIRAM)); the only static storage is the
//  context pointer. Locking: one recursive mutex (te_lock). The LVGL
//  task creates / ticks / deletes pages and loads / unloads themes;
//  httpd and storage jobs only call the sidecar functions, which
//  never call LVGL.
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include "theme_interface.h"
#include "theme_img_cache.h"
#include "theme_layout.h"
#include "theme_overlay.h"
#include "theme_pixconv.h"
#include "theme_sidecar_core.h"
#include "rules/tr_types.h"
#include "export_path/ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TE_TAG               "theme_engine"
#define TE_MANIFEST_SIZE     16384u          // theme.bin [0,16 KB) = manifest
#define TE_MAX_PAGES         32
#define TE_MAX_IMAGES        16
#define TE_MAX_FONTS         16
#define TE_LAYOUT_MAX        65536u          // one page layout
#define TE_ID_CAP            (THEME_ID_LEN + 1)   // editor ids are <= 64 chars
#define TE_TINT_BAKE_MAX     (128u * 1024u)  // image tint variants larger than this use runtime recolor
#define TE_PSRAM_RESERVE     (512u * 1024u)  // decorative images never leave less PSRAM free
#define TE_CHANGE_WINDOW_MS  280             // contract C4 change window
#define TE_REPORT_SKIP_MAX   8               // est.-skipped names listed per page

#define TE_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static inline void *te_malloc(size_t n) { return heap_caps_malloc(n ? n : 1, TE_CAPS); }
static inline void *te_calloc(size_t n, size_t sz)
{
    return heap_caps_calloc(n ? n : 1, sz ? sz : 1, TE_CAPS);
}
static inline void te_free(void *p) { heap_caps_free(p); }
char *te_strdup(const char *s);

// Allocator callbacks for the pure-C modules (tpx/tl/ov/tsc and tr).
void *te_alloc_cb(size_t size, void *ctx);
void  te_free_cb(void *ptr, void *ctx);
void *te_calloc_cb(size_t n, size_t size, void *ctx);
#define TE_ALLOC_INIT   {te_alloc_cb, te_free_cb, NULL}
#define TE_TR_ALLOC     {te_calloc_cb, te_free_cb, NULL}

typedef struct {
    char    *name;
    uint32_t offset, size;
    uint16_t w, h;
    uint8_t  fmt;             // tpx_fmt_t
} te_image_t;

typedef struct {
    char    *name;            // "font_<name>_<size>"
    uint32_t offset, size;
    lv_font_t *font;          // loaded lazily (lv_font_load), freed on release
    bool     failed;
} te_font_t;

typedef struct {
    char    *id;
    uint32_t offset, size;
} te_page_t;

typedef struct te_live_page te_live_page_t;   // theme_page.c

typedef struct {
    SemaphoreHandle_t lock;
    bool     loaded;          // a theme (custom or built-in default) is active
    bool     custom;          // selected SD manifest loaded
    theme_info_t info;
    char     id[TE_ID_CAP];   // full theme.id (info.id may be shorter)
    uint32_t colors[UI_COLOR__COUNT];
    uint32_t manifest_crc;

    const esp_partition_t *part; // retired runtime mapping fields; kept for layout compatibility
    uint32_t used_len;        // max(offset + size), >= TE_MANIFEST_SIZE
    const uint8_t *map;       // host fixture only; never populated by the firmware loader
    esp_partition_mmap_handle_t map_handle;

    uint8_t  n_images, n_fonts, n_pages;
    te_image_t images[TE_MAX_IMAGES];
    te_font_t  fonts[TE_MAX_FONTS];
    te_page_t  pages[TE_MAX_PAGES];

    char    *sc_json;         // sidecar of the current custom theme (NUL-terminated)
    size_t   sc_len;

    te_live_page_t *live;     // pages whose screen object exists
    te_live_page_t *active;   // cached page matching lv_scr_act(), if known
    uint8_t  n_live;
    bool     release_pending; // theme_release_resources() while pages were alive
    volatile bool  sweep_on;
    volatile float sweep;
    volatile uint16_t sweep_rpm;
    volatile bool  suppressed;

    tic_cache_t cache;
    lv_fs_drv_t fs_drv;       // 'F': fonts from the selected SD package
    bool     fs_registered;
    char     sd_path[128];   // runtime source; legacy mapping fields above are unused
    uint32_t file_size;
    uint32_t load_generation; // invalidates independently prepared pages on reload
    uint16_t n_preparing;     // keeps the SD package alive until staged jobs finish
} te_ctx_t;

extern te_ctx_t *g_te;        // NULL until the first engine / sidecar call

// Create the context on first use (atomic: sidecar installs may run on a
// storage task before theme_engine_init()). NULL only when out of memory.
te_ctx_t *te_ctx_ensure(void);

// The lock functions require g_te != NULL.
void te_lock(void);
void te_unlock(void);
bool te_trylock(void);

// ---- theme_loader.c (caller holds te_lock) ----
esp_err_t   te_read_from(te_ctx_t *c, uint32_t offset, void *dst, uint32_t size);
esp_err_t   te_read(uint32_t offset, void *dst, uint32_t size); // bounded SD segment read
int         te_image_find(const char *name);  // index or -1
lv_font_t  *te_font_get(const char *name);    // lazy lv_font_load, NULL if unavailable
int         te_page_find(const char *id);     // index or -1
tl_defaults_t te_layout_defaults(void);
void        te_release_locked(void);          // cache + fonts (no live pages)

// ---- theme_page.c ----
void te_page_on_destroy(te_live_page_t *p);   // LV_EVENT_DELETE of a page screen

// Page preparation shared by the page builder and the pairing report.
typedef struct {
    tl_layout_t  *layout;
    tsc_page_t   *sc;          // sidecar page (points into scratch), NULL if none
    char         *scratch;     // freed by te_prep_free
    tr_element_t *rel;         // bin elements the sidecar knows with the same kind
    uint16_t     *rel_el;      // rel[i] → layout index
    uint16_t      n_rel;
    tr_program_t *prog;        // NULL without rules
    tr_diag_t     diag;
    uint8_t      *is_target;   // [layout->n] 1 = some rule action targets it
} te_prep_t;

esp_err_t te_prep_page(int page_idx, bool with_sidecar, te_prep_t *out);
void      te_prep_free(te_prep_t *p);

bool      te_layout_has_video(const tl_layout_t *l);

// Pairing-report estimate of the page build's PSRAM plan: required bytes
// (pointers with their variants, rule-target images) and the decorative
// images the per-page budget would skip (skip_cb per image, top-most last).
typedef void (*te_skip_fn)(const char *elem_id, size_t cost, void *ctx);
size_t    te_estimate_page(const te_prep_t *p, te_skip_fn skip_cb, void *ctx);

// ---- theme_sidecar.c (caller holds te_lock) ----
void      te_sidecar_reload(void);            // after a custom theme load
void      te_sidecar_drop(void);

#ifdef __cplusplus
}
#endif
