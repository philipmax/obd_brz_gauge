// ============================================================
//  theme_layout.c — theme.bin page layout parser (see theme_layout.h)
// ============================================================

#include "theme_layout.h"
#include "theme_jsplit.h"
#include "rules/tr_parse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------- helpers ----------------

double tl_round(double v)
{
    return floor(v + 0.5);
}

int32_t tl_px(double v)
{
    if (isnan(v)) return 0;
    double r = floor(v + 0.5);
    if (r > TL_PX_MAX) return TL_PX_MAX;
    if (r < -TL_PX_MAX) return -TL_PX_MAX;
    return (int32_t)r;
}

bool tl_is_video_id(const char *id, const char **key)
{
    if (!id) return false;
    size_t skip = 0;
    if (strncmp(id, "vid_", 4) == 0) skip = 4;
    else if (strncmp(id, "video_", 6) == 0) skip = 6;
    else return false;
    if (key) *key = id + skip;
    return true;
}

static void *tl_alloc(const tl_alloc_t *a, size_t size)
{
    if (size == 0) size = 1;
    void *p = (a && a->alloc_fn) ? a->alloc_fn(size, a->ctx) : malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static void tl_free_p(const tl_alloc_t *a, const void *p)
{
    if (!p) return;
    if (a && a->alloc_fn) {
        if (a->free_fn) a->free_fn((void *)(uintptr_t)p, a->ctx);
        return;
    }
    free((void *)(uintptr_t)p);
}

static const char *tl_dup(const tl_alloc_t *a, const char *s, bool *nomem)
{
    size_t n = strlen(s) + 1;
    char *d = tl_alloc(a, n);
    if (!d) {
        *nomem = true;
        return NULL;
    }
    memcpy(d, s, n);
    return d;
}

static double tl_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

static const char *tl_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

static uint32_t tl_color(const cJSON *obj, const char *key, uint32_t def)
{
    uint32_t rgb;
    const char *s = tl_str(obj, key);
    return (s && tr_parse_color(s, &rgb)) ? rgb : def;
}

static void tl_elem_strings_free(const tl_alloc_t *a, tl_elem_t *e)
{
    tl_free_p(a, e->id);
    tl_free_p(a, e->data_source);
    tl_free_p(a, e->format);
    tl_free_p(a, e->text);
    tl_free_p(a, e->asset);
    tl_free_p(a, e->font_asset);
    memset(e, 0, sizeof(*e));
}

// ---------------- element ----------------

typedef struct {
    tl_layout_t *l;
    const tl_defaults_t *def;
    const tl_alloc_t *alloc;
    uint16_t bindings;
    bool nomem;
} tl_ctx_t;

static bool tl_grow(tl_ctx_t *c)
{
    tl_layout_t *l = c->l;
    if (l->n < l->cap) return true;
    uint16_t cap = l->cap ? (uint16_t)(l->cap * 2u) : 16u;
    if (cap > TL_MAX_ELEMENTS) cap = TL_MAX_ELEMENTS;
    tl_elem_t *el = tl_alloc(c->alloc, (size_t)cap * sizeof(tl_elem_t));
    if (!el) return false;
    if (l->el) {
        memcpy(el, l->el, (size_t)l->n * sizeof(tl_elem_t));
        tl_free_p(c->alloc, l->el);
    }
    l->el = el;
    l->cap = cap;
    return true;
}

static void tl_add_element(tl_ctx_t *c, const cJSON *o)
{
    tl_layout_t *l = c->l;
    const char *type = tl_str(o, "type");
    tr_kind_t kind = tr_kind_from_str(type);
    if (kind == TR_KIND_UNKNOWN) {
        if (l->unknown_types++ == 0) {
            const char *t = type ? type : "(none)";
            size_t n = strlen(t);
            if (n >= sizeof(l->first_unknown)) n = sizeof(l->first_unknown) - 1;
            memcpy(l->first_unknown, t, n);
            l->first_unknown[n] = '\0';
        }
        return;
    }
    if (l->n >= TL_MAX_ELEMENTS) {
        l->over_limit++;
        return;
    }
    if (!tl_grow(c)) {
        c->nomem = true;
        return;
    }

    tl_elem_t e;
    memset(&e, 0, sizeof(e));
    bool nomem = false;
    const char *id = tl_str(o, "id");
    e.id = tl_dup(c->alloc, id ? id : "", &nomem);
    e.kind = (uint8_t)kind;
    e.x = tl_num(o, "x", 0);
    e.y = tl_num(o, "y", 0);
    e.width = tl_num(o, "width", 0);
    e.height = tl_num(o, "height", 0);
    e.start_angle = tl_num(o, "start_angle", 0);
    e.end_angle = tl_num(o, "end_angle", 0);
    e.rotation = tl_num(o, "rotation", 0);
    e.range_min = tl_num(o, "range_min", 0);
    e.range_max = tl_num(o, "range_max", 100);
    e.angle_min = tl_num(o, "angle_min", 0);
    e.angle_max = tl_num(o, "angle_max", 0);
    e.pivot_x = tl_num(o, "pivot_x", 50);
    e.pivot_y = tl_num(o, "pivot_y", 50);
    e.line_width = tl_num(o, "line_width", 0);
    e.radius = tl_num(o, "radius", 0);
    e.divisor = tl_num(o, "divisor", 0);
    e.font_size = tl_num(o, "font_size", 16);
    e.opacity = fmax(0.0, fmin(100.0, tl_num(o, "opacity", 100)));
    e.bg_opacity = fmax(0.0, fmin(100.0, tl_num(o, "bg_opacity", 100)));

    if (kind == TR_KIND_LABEL) {
        e.color = tl_color(o, "color", c->def->text_primary);
        e.bg_color = c->def->text_primary;
    } else {
        e.color = tl_color(o, "color", c->def->arc_indicator);
        e.bg_color = tl_color(o, "bg_color", c->def->arc_track);
    }

    const char *al = tl_str(o, "align");
    if (al && strcmp(al, "center") == 0) e.align = TL_ALIGN_CENTER;
    else if (al && strcmp(al, "right") == 0) e.align = TL_ALIGN_RIGHT;
    else e.align = TL_ALIGN_LEFT;

    e.video = (kind == TR_KIND_IMAGE && tl_is_video_id(e.id, NULL)) ? 1 : 0;
    const char *playback = tl_str(o, "video_playback");
    e.video_value = e.video && playback && strcmp(playback, "value") == 0 &&
                    isfinite(e.range_min) && isfinite(e.range_max) && e.range_max > e.range_min;
    e.video_step = tl_num(o, "video_step", 0);
    if (!isfinite(e.video_step) || e.video_step < 0) e.video_step = 0;
    const char *ds = tl_str(o, "data_source");
    if (ds && ds[0] && (kind != TR_KIND_IMAGE || e.video_value)) {
        if (c->bindings < TL_MAX_BINDINGS) {
            e.data_source = tl_dup(c->alloc, ds, &nomem);
            c->bindings++;
        } else {
            l->bindings_dropped++;
        }
    }
    const char *s = tl_str(o, "format");
    if (s) e.format = tl_dup(c->alloc, s, &nomem);
    s = tl_str(o, "text");
    if (s) e.text = tl_dup(c->alloc, s, &nomem);
    s = tl_str(o, "asset");
    if (s && s[0]) e.asset = tl_dup(c->alloc, s, &nomem);
    s = tl_str(o, "font_asset");
    if (s && s[0]) e.font_asset = tl_dup(c->alloc, s, &nomem);

    if (nomem) {
        tl_elem_strings_free(c->alloc, &e);
        c->nomem = true;
        return;
    }
    l->el[l->n++] = e;
}

// ---------------- root ----------------

static bool tl_cb(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                  const char *json, size_t len, bool overflow)
{
    (void)index;
    tl_ctx_t *c = ctx;
    if (overflow || !json) {
        if (kind == TJS_ITEM) c->l->invalid++;
        return true;
    }
    cJSON *o = cJSON_ParseWithLength(json, len);
    if (kind == TJS_ITEM) {
        if (cJSON_IsObject(o)) tl_add_element(c, o);
        else c->l->invalid++;
    } else if (strcmp(key, "background_color") == 0) {
        uint32_t rgb;
        if (cJSON_IsString(o) && o->valuestring && tr_parse_color(o->valuestring, &rgb)) {
            c->l->bg_color = rgb;
        }
    }
    cJSON_Delete(o);
    return !c->nomem;
}

tl_layout_t *tl_parse(const char *json, size_t len, const tl_defaults_t *def,
                      const tl_alloc_t *alloc, char *scratch, size_t scratch_cap)
{
    if (!json || !def || !scratch || scratch_cap == 0) return NULL;
    tl_layout_t *l = tl_alloc(alloc, sizeof(tl_layout_t));
    if (!l) return NULL;
    l->bg_color = def->bg;

    static const char *const capture[] = {"background_color", NULL};
    static const char *const split[] = {"elements", NULL};
    tl_ctx_t c = {l, def, alloc, 0, false};
    tjs_cfg_t cfg = {capture, split, 0, scratch, scratch_cap, tl_cb, &c};
    bool ok = tjs_run(&cfg, json, len);
    if (c.nomem || (!ok && l->n == 0 && l->invalid == 0 && l->unknown_types == 0)) {
        tl_free(l, alloc);
        return NULL;
    }
    return l;
}

void tl_free(tl_layout_t *l, const tl_alloc_t *alloc)
{
    if (!l) return;
    for (uint16_t i = 0; i < l->n; i++) tl_elem_strings_free(alloc, &l->el[i]);
    tl_free_p(alloc, l->el);
    tl_free_p(alloc, l);
}
