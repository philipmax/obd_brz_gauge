#pragma once
// ============================================================
//  theme_img_cache.h — converted theme images (PSRAM, pinned LRU)
//
//  REFACTOR_PLAN.md 5.7 / P4: key = (manifest CRC, asset index,
//  variant). An entry holds one LVGL image descriptor whose pixels
//  are owned in PSRAM. Runtime theme assets are read from SD in
//  bounded segments; there is no Flash theme cache.
//
//  Pages pin the entries they draw (tic_lookup / tic_insert return a
//  pinned entry, tic_release unpins). Unpinned entries stay cached
//  until LRU eviction (tic_make_room / tic_trim) or tic_clear().
//  tic_clear() frees unpinned entries and dooms pinned ones (freed on
//  their last release), so a descriptor LVGL may still draw is never
//  freed under it.
//
//  Not thread-safe: the theme engine calls it under its own lock.
//  Nothing static; entries live in PSRAM.
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "theme_pixconv.h"

#ifdef __cplusplus
extern "C" {
#endif

// Per-page budget of converted pixels (plan 5.7 PSRAM table). Entries a
// page reuses (cache hits) count against it too; zero-copy costs 0.
#define TIC_BUDGET_NO_VIDEO     (2560u * 1024u)
#define TIC_BUDGET_WITH_VIDEO   (1536u * 1024u)

typedef enum {
    TIC_KIND_IMAGE = 0,       // image element: window + crop, no scaling
    TIC_KIND_POINTER = 1,     // pointer: resampled to dw x dh, cropped around the pivot
} tic_kind_t;

// Everything that changes the produced pixels. Always memset() before
// filling: the cache compares variants with memcmp().
typedef struct {
    uint8_t  kind;            // tic_kind_t
    uint8_t  crop;            // tpx_crop_t (images)
    uint8_t  round_dc;        // round-screen don't-care (unrotated images)
    uint8_t  has_tint;        // tint baked in (TR_RECOLOR_OPA)
    uint32_t tint;            // 0xRRGGBB
    int32_t  win_w, win_h;    // image window from source (0,0)
    int32_t  scr_x, scr_y;    // screen position of source (0,0) (round_dc)
    uint16_t dw, dh;          // pointer resample size
    int32_t  piv_x, piv_y;    // pointer pivot inside the resampled box
} tic_variant_t;

typedef struct tic_entry {
    struct tic_entry *next;
    uint32_t crc;
    uint16_t asset;
    tic_variant_t var;
    lv_img_dsc_t dsc;         // stable address: handed to lv_img_set_src
    int32_t  ox, oy;          // source/box coords of output pixel (0,0)
    size_t   cost;            // PSRAM bytes owned (0 for zero-copy / empty)
    uint8_t *owned;
    uint16_t refs;            // pins
    uint32_t stamp;           // LRU clock
    bool     doomed;          // cleared while pinned: freed on last release
    bool     empty;           // nothing visible (fully transparent window)
} tic_entry_t;

typedef struct {
    tic_entry_t *head;
    size_t   used;            // bytes owned by all entries
    size_t   pinned;          // bytes owned by pinned entries
    uint16_t count;
    uint32_t clock;
    uint32_t hits, misses, evictions;
    bool unpublished;         // worker-owned descriptors never passed to LVGL
} tic_cache_t;

void tic_init(tic_cache_t *c);

// LVGL task only: move unpublished entries into the live cache.
void tic_adopt(tic_cache_t *dst, tic_cache_t *src);

// Cached, non-doomed entry → pinned; NULL on miss.
tic_entry_t *tic_lookup(tic_cache_t *c, uint32_t crc, uint16_t asset, const tic_variant_t *v);

// Best effort: evict least recently used unpinned entries until
// used + need <= cap.
void tic_make_room(tic_cache_t *c, size_t need, size_t cap);

// Add a rendered image (takes ownership of img->owned). Returns the
// pinned entry, or NULL on allocation failure (img->owned freed).
tic_entry_t *tic_insert(tic_cache_t *c, uint32_t crc, uint16_t asset, const tic_variant_t *v,
                        tpx_img_t *img);

void tic_release(tic_cache_t *c, tic_entry_t *e);

// Evict unpinned entries (LRU first) until used <= cap.
void tic_trim(tic_cache_t *c, size_t cap);

// Free every unpinned entry; doom pinned ones.
void tic_clear(tic_cache_t *c);

#ifdef __cplusplus
}
#endif
