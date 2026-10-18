// ============================================================
//  theme_img_cache.c — converted theme images (see theme_img_cache.h)
// ============================================================

#include "theme_img_cache.h"

#include <string.h>
#include "esp_heap_caps.h"

#define TIC_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void tic_init(tic_cache_t *c)
{
    if (c) memset(c, 0, sizeof(*c));
}

void tic_adopt(tic_cache_t *dst, tic_cache_t *src)
{
    while (src->head) {
        tic_entry_t *e = src->head;
        src->head = e->next;
        e->stamp = ++dst->clock;
        e->next = dst->head;
        dst->head = e;
    }
    dst->used += src->used;
    dst->pinned += src->pinned;
    dst->count += src->count;
    tic_init(src);
    src->unpublished = true;
}

static void tic_free_entry(tic_cache_t *c, tic_entry_t *e)
{
    if (!c->unpublished && !e->empty) lv_img_cache_invalidate_src(&e->dsc);   // zero-copy too: the address may be reused
    if (e->owned) heap_caps_free(e->owned);
    c->used -= e->cost;
    c->count--;
    heap_caps_free(e);
}

// Unlink and free *link (an unpinned entry).
static void tic_unlink(tic_cache_t *c, tic_entry_t **link)
{
    tic_entry_t *e = *link;
    *link = e->next;
    tic_free_entry(c, e);
}

static void tic_pin(tic_cache_t *c, tic_entry_t *e)
{
    if (e->refs++ == 0) c->pinned += e->cost;
    e->stamp = ++c->clock;
}

tic_entry_t *tic_lookup(tic_cache_t *c, uint32_t crc, uint16_t asset, const tic_variant_t *v)
{
    if (!c || !v) return NULL;
    for (tic_entry_t *e = c->head; e; e = e->next) {
        if (e->doomed || e->crc != crc || e->asset != asset) continue;
        if (memcmp(&e->var, v, sizeof(*v)) != 0) continue;
        tic_pin(c, e);
        c->hits++;
        return e;
    }
    c->misses++;
    return NULL;
}

// Least recently used unpinned entry (link to it), NULL when none.
static tic_entry_t **tic_lru(tic_cache_t *c)
{
    tic_entry_t **best = NULL;
    for (tic_entry_t **l = &c->head; *l; l = &(*l)->next) {
        if ((*l)->refs) continue;
        if (!best || (*l)->stamp < (*best)->stamp) best = l;
    }
    return best;
}

void tic_make_room(tic_cache_t *c, size_t need, size_t cap)
{
    if (!c) return;
    while (c->used + need > cap) {
        tic_entry_t **l = tic_lru(c);
        if (!l) break;
        tic_unlink(c, l);
        c->evictions++;
    }
}

void tic_trim(tic_cache_t *c, size_t cap)
{
    tic_make_room(c, 0, cap);
}

tic_entry_t *tic_insert(tic_cache_t *c, uint32_t crc, uint16_t asset, const tic_variant_t *v,
                        tpx_img_t *img)
{
    if (!c || !v || !img) return NULL;
    tic_entry_t *e = heap_caps_calloc(1, sizeof(*e), TIC_CAPS);
    if (!e) {
        heap_caps_free(img->owned);
        memset(img, 0, sizeof(*img));
        return NULL;
    }
    const tpx_plan_t *p = &img->plan;
    e->crc = crc;
    e->asset = asset;
    e->var = *v;
    e->ox = p->ox;
    e->oy = p->oy;
    e->owned = img->owned;
    e->cost = img->owned ? p->size : 0;
    e->empty = p->cf == TPX_CF_NONE || !img->data;
    if (!e->empty) {
        e->dsc.header.always_zero = 0;
        e->dsc.header.cf = p->cf == TPX_CF_RGB565A8 ? LV_IMG_CF_RGB565A8 : LV_IMG_CF_TRUE_COLOR;
        e->dsc.header.w = p->w;
        e->dsc.header.h = p->h;
        e->dsc.data_size = (uint32_t)p->size;
        e->dsc.data = img->data;
    }
    memset(img, 0, sizeof(*img));

    e->next = c->head;
    c->head = e;
    c->used += e->cost;
    c->count++;
    tic_pin(c, e);
    return e;
}

void tic_release(tic_cache_t *c, tic_entry_t *e)
{
    if (!c || !e || e->refs == 0) return;
    if (--e->refs) return;
    c->pinned -= e->cost;
    if (!e->doomed) return;
    for (tic_entry_t **l = &c->head; *l; l = &(*l)->next) {
        if (*l == e) {
            tic_unlink(c, l);
            return;
        }
    }
}

void tic_clear(tic_cache_t *c)
{
    if (!c) return;
    tic_entry_t **l = &c->head;
    while (*l) {
        if ((*l)->refs) {
            (*l)->doomed = true;
            l = &(*l)->next;
        } else {
            tic_unlink(c, l);
        }
    }
}
