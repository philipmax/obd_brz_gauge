// ============================================================
//  theme_sidecar_core.c — sidecar reduce + pairing (see theme_sidecar_core.h)
// ============================================================

#include "theme_sidecar_core.h"
#include "theme_jsplit.h"
#include "rules/tr_parse.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TSC_MAX_ASSET_FILES  64u
#define TSC_NAME_MAX         128u    // asset name / fileName
#define TSC_KIND_MAX         32u

// ---------------- allocation ----------------

static void *a_alloc(const tsc_alloc_t *a, size_t n)
{
    return (a && a->alloc_fn) ? a->alloc_fn(n, a->ctx) : malloc(n);
}

static void a_free(const tsc_alloc_t *a, void *p)
{
    if (!p) return;
    if (a && a->alloc_fn) {
        if (a->free_fn) a->free_fn(p, a->ctx);
    } else {
        free(p);
    }
}

// ---------------- growable string ----------------

typedef struct {
    char *p;
    size_t len, cap, max;
    bool overflow, nomem;
    const tsc_alloc_t *a;
} dbuf_t;

static void db_init(dbuf_t *d, const tsc_alloc_t *a, size_t max)
{
    memset(d, 0, sizeof(*d));
    d->a = a;
    d->max = max;
}

static void db_free(dbuf_t *d)
{
    a_free(d->a, d->p);
    d->p = NULL;
    d->len = d->cap = 0;
}

static void db_reset(dbuf_t *d)
{
    d->len = 0;
    d->overflow = false;
    if (d->p) d->p[0] = '\0';
}

static bool db_put(dbuf_t *d, const char *s, size_t n)
{
    if (d->overflow || d->nomem) return false;
    if (d->len + n > d->max) {
        d->overflow = true;
        return false;
    }
    if (d->len + n + 1 > d->cap) {
        size_t nc = d->cap ? d->cap : 256;
        while (nc < d->len + n + 1) nc *= 2;
        if (nc > d->max + 1) nc = d->max + 1;
        char *np = a_alloc(d->a, nc);
        if (!np) {
            d->nomem = true;
            return false;
        }
        if (d->len) memcpy(np, d->p, d->len);
        a_free(d->a, d->p);
        d->p = np;
        d->cap = nc;
    }
    memcpy(d->p + d->len, s, n);
    d->len += n;
    d->p[d->len] = '\0';
    return true;
}

static bool db_str(dbuf_t *d, const char *s)
{
    return db_put(d, s, strlen(s));
}

// JSON string literal of s (UTF-8 passed through).
static bool db_jstr(dbuf_t *d, const char *s)
{
    if (!db_put(d, "\"", 1)) return false;
    const char *run = s;
    for (const char *c = s; ; c++) {
        unsigned char u = (unsigned char)*c;
        if (u != 0 && u >= 0x20 && u != '"' && u != '\\') continue;
        if (c > run && !db_put(d, run, (size_t)(c - run))) return false;
        if (u == 0) break;
        char esc[8];
        if (u == '"') memcpy(esc, "\\\"", 3);
        else if (u == '\\') memcpy(esc, "\\\\", 3);
        else if (u == '\n') memcpy(esc, "\\n", 3);
        else if (u == '\r') memcpy(esc, "\\r", 3);
        else if (u == '\t') memcpy(esc, "\\t", 3);
        else snprintf(esc, sizeof(esc), "\\u%04x", u);
        if (!db_str(d, esc)) return false;
        run = c + 1;
    }
    return db_put(d, "\"", 1);
}

// ---------------- small helpers ----------------

// JSON.parse semantics: last duplicate key wins.
static const cJSON *jget(const cJSON *obj, const char *key)
{
    const cJSON *hit = NULL;
    if (!cJSON_IsObject(obj)) return NULL;
    for (const cJSON *c = obj->child; c; c = c->next) {
        if (c->string && strcmp(c->string, key) == 0) hit = c;
    }
    return hit;
}

static const char *jstr(const cJSON *obj, const char *key)
{
    const cJSON *it = jget(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

// Copy with truncation on a UTF-8 boundary.
static void copy_trunc(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t l = src ? strlen(src) : 0;
    if (l >= cap) {
        l = cap - 1;
        while (l > 0 && ((unsigned char)src[l] & 0xC0) == 0x80) l--;
    }
    if (l) memcpy(dst, src, l);
    dst[l] = '\0';
}

static bool id_ok(const char *s, size_t max)
{
    if (!s || !s[0]) return false;
    return strlen(s) < max;
}

bool tsc_valid_theme_id(const char *id)
{
    if (!id) return false;
    char c = id[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    size_t n = 1;
    for (const char *p = id + 1; *p; p++, n++) {
        c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok || n >= 64) return false;
    }
    return true;
}

// ---------------- builder ----------------

typedef struct {
    size_t end;           // offset in pages dbuf after this page
    uint16_t rules, elements, overlays;
    char id[TSC_ID_MAX];
} page_rec_t;

struct tsc_builder {
    tsc_alloc_t a;
    tjs_t top;
    char *cap_buf;        // top-level captures (one page / meta / asset item)
    char *item_buf;       // nested captures inside one page

    bool error, nomem, format_bad, is_reduced, have_tid, have_meta_id;
    char tid[72], ver[32], meta_id[72], meta_ver[32];
    bool have_ver, have_meta_ver;

    dbuf_t source;        // reduced input: "source" object text
    dbuf_t assets;        // "name":"file",... (no braces)
    uint16_t n_assets;
    dbuf_t pages;         // {...},{...}
    page_rec_t rec[TSC_MAX_PAGES];
    uint16_t n_pages;

    // current page
    char pid[TSC_ID_MAX];
    bool pid_ok, ids_given;
    dbuf_t ids, ids_given_buf, elems, ovl, rules, page;
    uint16_t n_elem, n_rules, n_ovl, n_ids_given;

    tsc_stats_t st;
};

static void b_oom(tsc_builder_t *b, const dbuf_t *d)
{
    if (d->nomem) b->nomem = true;
}

static cJSON *parse(const char *json, size_t len)
{
    return json ? cJSON_ParseWithLength(json, len) : NULL;
}

// kind-specific overlay entry of one element (gaugeproj element or reduced
// overlay entry). Returns true when an entry was written.
static bool b_overlay(tsc_builder_t *b, const cJSON *el, const char *id, const char *kind_str)
{
    static const char *const dd_keys[] = {"scale", "offset", "inputMin", "inputMax", "displayMin",
                                          "displayMax", "precision", "unit", "valueMap"};
    if (!kind_str) return false;
    tr_kind_t kind = tr_kind_from_str(kind_str);
    if (kind == TR_KIND_UNKNOWN || kind == TR_KIND_IMAGE) return false;

    const cJSON *rot = jget(el, "rotation");
    const cJSON *dir = jget(el, "direction");
    const cJSON *dd = jget(el, "dataDisplay");
    bool want_rot = (kind == TR_KIND_LABEL || kind == TR_KIND_BAR) && cJSON_IsNumber(rot) &&
                    rot->valuedouble != 0;
    bool want_dir = kind == TR_KIND_BAR && cJSON_IsString(dir);
    bool want_dd = false;
    if (cJSON_IsObject(dd)) {
        for (size_t i = 0; i < sizeof(dd_keys) / sizeof(dd_keys[0]); i++) {
            if (jget(dd, dd_keys[i])) { want_dd = true; break; }
        }
    }
    if (!want_rot && !want_dir && !want_dd) return false;

    cJSON *o = cJSON_CreateObject();
    if (!o) { b->nomem = true; return false; }
    bool ok = cJSON_AddStringToObject(o, "kind", kind_str) != NULL;
    if (ok && want_rot) ok = cJSON_AddNumberToObject(o, "rotation", rot->valuedouble) != NULL;
    if (ok && want_dir) ok = cJSON_AddStringToObject(o, "direction", dir->valuestring) != NULL;
    if (ok && want_dd) {
        cJSON *f = cJSON_AddObjectToObject(o, "dataDisplay");
        ok = f != NULL;
        for (size_t i = 0; ok && i < sizeof(dd_keys) / sizeof(dd_keys[0]); i++) {
            const cJSON *v = jget(dd, dd_keys[i]);
            if (!v) continue;
            cJSON *dup = cJSON_Duplicate(v, true);
            ok = dup && cJSON_AddItemToObject(f, dd_keys[i], dup);
            if (!ok) cJSON_Delete(dup);
        }
    }
    char *txt = ok ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!txt) { b->nomem = true; return false; }

    bool w = true;
    if (b->ovl.len) w = db_put(&b->ovl, ",", 1);
    w = w && db_jstr(&b->ovl, id) && db_put(&b->ovl, ":", 1) && db_str(&b->ovl, txt);
    cJSON_free(txt);
    b_oom(b, &b->ovl);
    if (w) b->n_ovl++;
    return w;
}

static bool b_element(tsc_builder_t *b, const char *id, const char *kind)
{
    if (!id_ok(id, TSC_ID_MAX) || !kind || strlen(kind) >= TSC_KIND_MAX) return false;
    if (b->n_elem >= TSC_MAX_ELEMENTS) {
        b->st.elements_truncated++;
        return false;
    }
    bool w = true;
    if (b->elems.len) w = db_put(&b->elems, ",", 1);
    w = w && db_jstr(&b->elems, id) && db_put(&b->elems, ":", 1) && db_jstr(&b->elems, kind);
    if (b->ids.len) w = w && db_put(&b->ids, ",", 1);
    w = w && db_jstr(&b->ids, id);
    b_oom(b, &b->elems);
    b_oom(b, &b->ids);
    if (w) b->n_elem++;
    return w;
}

static bool b_page_cb(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                      const char *json, size_t len, bool overflow)
{
    (void)index;
    tsc_builder_t *b = ctx;
    if (overflow) {
        if (strcmp(key, "rules") == 0) b->st.rules_truncated++;
        else if (strcmp(key, "elements") == 0) b->st.elements_truncated++;
        return true;
    }
    if (kind == TJS_ITEM && strcmp(key, "rules") == 0) {
        if (b->n_rules >= TR_MAX_RULES) {
            b->st.rules_truncated++;
            return true;
        }
        cJSON *r = parse(json, len);
        bool obj = cJSON_IsObject(r);
        cJSON_Delete(r);
        if (!obj) return true;
        bool w = true;
        if (b->rules.len) w = db_put(&b->rules, ",", 1);
        w = w && db_put(&b->rules, json, len);
        b_oom(b, &b->rules);
        if (w) b->n_rules++;
        else b->st.rules_truncated++;
        return !b->nomem;
    }
    if (kind == TJS_ITEM && strcmp(key, "elements") == 0) {
        // gaugeproj GaugeElement
        cJSON *el = parse(json, len);
        const char *id = jstr(el, "id");
        const char *k = jstr(el, "kind");
        if (b_element(b, id, k)) b_overlay(b, el, id, k);
        cJSON_Delete(el);
        return !b->nomem;
    }
    if (kind != TJS_VALUE) return true;

    cJSON *v = parse(json, len);
    if (strcmp(key, "id") == 0) {
        if (cJSON_IsString(v) && id_ok(v->valuestring, TSC_ID_MAX)) {
            copy_trunc(b->pid, sizeof(b->pid), v->valuestring);
            b->pid_ok = true;
        }
    } else if (strcmp(key, "element_ids") == 0 && cJSON_IsArray(v)) {
        b->ids_given = true;
        for (const cJSON *c = v->child; c; c = c->next) {
            if (!cJSON_IsString(c) || !id_ok(c->valuestring, TSC_ID_MAX)) continue;
            if (b->n_ids_given >= TSC_MAX_ELEMENTS) break;
            bool w = true;
            if (b->ids_given_buf.len) w = db_put(&b->ids_given_buf, ",", 1);
            w = w && db_jstr(&b->ids_given_buf, c->valuestring);
            b_oom(b, &b->ids_given_buf);
            if (w) b->n_ids_given++;
        }
    } else if (strcmp(key, "elements") == 0 && cJSON_IsObject(v)) {
        // reduced form: {"id":"kind"}
        for (const cJSON *c = v->child; c; c = c->next) {
            if (cJSON_IsString(c)) b_element(b, c->string, c->valuestring);
        }
    } else if (strcmp(key, "overlay") == 0 && cJSON_IsObject(v)) {
        for (const cJSON *c = v->child; c; c = c->next) {
            if (cJSON_IsObject(c) && id_ok(c->string, TSC_ID_MAX)) {
                b_overlay(b, c, c->string, jstr(c, "kind"));
            }
        }
    }
    cJSON_Delete(v);
    return !b->nomem;
}

static void b_page(tsc_builder_t *b, const char *json, size_t len)
{
    if (b->n_pages >= TSC_MAX_PAGES) {
        b->st.pages_over_limit++;
        return;
    }
    b->pid[0] = '\0';
    b->pid_ok = b->ids_given = false;
    b->n_elem = b->n_rules = b->n_ovl = b->n_ids_given = 0;
    db_reset(&b->ids);
    db_reset(&b->ids_given_buf);
    db_reset(&b->elems);
    db_reset(&b->ovl);
    db_reset(&b->rules);
    db_reset(&b->page);

    static const char *const cap[] = {"id", "element_ids", "elements", "overlay", NULL};
    static const char *const split[] = {"elements", "rules", NULL};
    tjs_cfg_t cfg = {
        .capture_keys = cap, .split_keys = split, .max_str = TSC_MAX_STR,
        .buf = b->item_buf, .cap = TSC_ITEM_MAX, .cb = b_page_cb, .ctx = b,
    };
    tjs_t s;
    tjs_init(&s, &cfg);
    tjs_feed(&s, json, len);
    if (b->nomem) return;
    if (!tjs_finish(&s) || !b->pid_ok) {
        b->st.pages_invalid++;
        return;
    }
    for (uint16_t i = 0; i < b->n_pages; i++) {
        if (strcmp(b->rec[i].id, b->pid) == 0) {   // duplicate id: first wins
            b->st.pages_invalid++;
            return;
        }
    }
    if (b->ids.overflow || b->elems.overflow || b->ovl.overflow || b->rules.overflow ||
        b->ids_given_buf.overflow) {
        b->st.pages_too_big++;
        return;
    }

    const dbuf_t *ids = b->ids_given ? &b->ids_given_buf : &b->ids;
    dbuf_t *p = &b->page;
    bool w = db_str(p, "{\"id\":") && db_jstr(p, b->pid) &&
             db_str(p, ",\"element_ids\":[") && db_put(p, ids->p ? ids->p : "", ids->len) &&
             db_str(p, "],\"elements\":{") && db_put(p, b->elems.p ? b->elems.p : "", b->elems.len) &&
             db_str(p, "},\"overlay\":{") && db_put(p, b->ovl.p ? b->ovl.p : "", b->ovl.len) &&
             db_str(p, "},\"rules\":[") && db_put(p, b->rules.p ? b->rules.p : "", b->rules.len) &&
             db_str(p, "]}");
    b_oom(b, p);
    if (!w) {
        if (!b->nomem) b->st.pages_too_big++;
        return;
    }
    // Pre-check so a page that does not fit leaves no partial write (and a
    // later, smaller page may still fit).
    if (b->pages.len + (b->pages.len ? 1u : 0u) + p->len > b->pages.max) {
        b->st.pages_too_big++;
        return;
    }
    w = (!b->pages.len || db_put(&b->pages, ",", 1)) && db_put(&b->pages, p->p, p->len);
    b_oom(b, &b->pages);
    if (!w) {
        if (b->n_pages) b->pages.len = b->rec[b->n_pages - 1].end;
        else b->pages.len = 0;
        if (!b->nomem) b->st.pages_too_big++;
        return;
    }
    page_rec_t *r = &b->rec[b->n_pages++];
    r->end = b->pages.len;
    r->rules = b->n_rules;
    r->elements = b->n_elem;
    r->overlays = b->n_ovl;
    copy_trunc(r->id, sizeof(r->id), b->pid);
    b->st.pages++;
    b->st.rules += b->n_rules;
    b->st.elements += b->n_elem;
    b->st.overlays += b->n_ovl;
}

static void b_asset_pair(tsc_builder_t *b, const char *name, const char *file)
{
    if (!id_ok(name, TSC_NAME_MAX) || !id_ok(file, TSC_NAME_MAX)) return;
    if (b->n_assets >= TSC_MAX_ASSET_FILES) return;
    bool w = (!b->assets.len || db_put(&b->assets, ",", 1)) && db_jstr(&b->assets, name) &&
             db_put(&b->assets, ":", 1) && db_jstr(&b->assets, file);
    b_oom(b, &b->assets);
    if (w) b->n_assets++;
}

static bool b_top_cb(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                     const char *json, size_t len, bool overflow)
{
    (void)index;
    tsc_builder_t *b = ctx;
    if (kind == TJS_ITEM && strcmp(key, "pages") == 0) {
        if (overflow) {
            if (b->n_pages >= TSC_MAX_PAGES) b->st.pages_over_limit++;
            else b->st.pages_too_big++;
            return true;
        }
        b_page(b, json, len);
        return !b->nomem;
    }
    if (overflow) return true;
    cJSON *v = parse(json, len);
    if (kind == TJS_ITEM && strcmp(key, "assets") == 0) {
        b_asset_pair(b, jstr(v, "name"), jstr(v, "fileName"));
    } else if (kind == TJS_VALUE) {
        if (strcmp(key, "format") == 0) {
            if (cJSON_IsString(v) && strcmp(v->valuestring, TSC_FORMAT) == 0) b->is_reduced = true;
            else b->format_bad = true;
        } else if (strcmp(key, "theme_id") == 0 && cJSON_IsString(v)) {
            b->have_tid = strlen(v->valuestring) < sizeof(b->tid);
            if (b->have_tid) copy_trunc(b->tid, sizeof(b->tid), v->valuestring);
        } else if (strcmp(key, "project_version") == 0 && cJSON_IsString(v)) {
            copy_trunc(b->ver, sizeof(b->ver), v->valuestring);
            b->have_ver = true;
        } else if (strcmp(key, "meta") == 0 && cJSON_IsObject(v)) {
            const char *id = jstr(v, "id");
            const char *ver = jstr(v, "version");
            if (id && strlen(id) < sizeof(b->meta_id)) {
                copy_trunc(b->meta_id, sizeof(b->meta_id), id);
                b->have_meta_id = true;
            }
            if (ver) {
                copy_trunc(b->meta_ver, sizeof(b->meta_ver), ver);
                b->have_meta_ver = true;
            }
        } else if (strcmp(key, "source") == 0 && cJSON_IsObject(v)) {
            db_reset(&b->source);
            db_put(&b->source, json, len);
            b_oom(b, &b->source);
        } else if (strcmp(key, "asset_files") == 0 && cJSON_IsObject(v)) {
            for (const cJSON *c = v->child; c; c = c->next) {
                if (cJSON_IsString(c)) b_asset_pair(b, c->string, c->valuestring);
            }
        }
    }
    cJSON_Delete(v);
    return !b->nomem;
}

tsc_builder_t *tsc_builder_new(const tsc_alloc_t *alloc)
{
    tsc_alloc_t a = {0};
    if (alloc) a = *alloc;
    tsc_builder_t *b = a_alloc(&a, sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->a = a;
    b->cap_buf = a_alloc(&b->a, TSC_CAPTURE_MAX);
    b->item_buf = a_alloc(&b->a, TSC_ITEM_MAX);
    if (!b->cap_buf || !b->item_buf) {
        tsc_builder_free(b);
        return NULL;
    }
    db_init(&b->source, &b->a, 1024);
    db_init(&b->assets, &b->a, TSC_MAX_BYTES);
    db_init(&b->pages, &b->a, TSC_MAX_BYTES);
    db_init(&b->ids, &b->a, TSC_MAX_BYTES);
    db_init(&b->ids_given_buf, &b->a, TSC_MAX_BYTES);
    db_init(&b->elems, &b->a, TSC_MAX_BYTES);
    db_init(&b->ovl, &b->a, TSC_MAX_BYTES);
    db_init(&b->rules, &b->a, TSC_MAX_BYTES);
    db_init(&b->page, &b->a, TSC_MAX_BYTES);

    static const char *const cap[] = {"format", "theme_id", "project_version", "meta",
                                      "source", "asset_files", NULL};
    static const char *const split[] = {"pages", "assets", NULL};
    tjs_cfg_t cfg = {
        .capture_keys = cap, .split_keys = split, .max_str = TSC_MAX_STR,
        .buf = b->cap_buf, .cap = TSC_CAPTURE_MAX, .cb = b_top_cb, .ctx = b,
    };
    tjs_init(&b->top, &cfg);
    return b;
}

bool tsc_builder_feed(tsc_builder_t *b, const char *data, size_t len)
{
    if (!b || b->error || b->nomem) return false;
    if (!tjs_feed(&b->top, data, len)) {
        b->error = true;
        return false;
    }
    return true;
}

tsc_err_t tsc_builder_finish(tsc_builder_t *b, uint64_t src_size, const char *sha256_hex,
                             char **out, size_t *out_len, tsc_stats_t *st)
{
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!b || !out) return TSC_ERR_JSON;
    tsc_err_t err = TSC_OK;
    if (b->nomem) err = TSC_ERR_NOMEM;
    else if (b->error || !tjs_finish(&b->top)) err = TSC_ERR_JSON;
    else if (b->format_bad) err = TSC_ERR_FORMAT;

    const char *tid = b->is_reduced ? (b->have_tid ? b->tid : NULL)
                                    : (b->have_meta_id ? b->meta_id : (b->have_tid ? b->tid : NULL));
    const char *ver = b->is_reduced ? (b->have_ver ? b->ver : "")
                                    : (b->have_meta_ver ? b->meta_ver : (b->have_ver ? b->ver : ""));
    b->st.reduced_input = b->is_reduced;
    copy_trunc(b->st.theme_id, sizeof(b->st.theme_id), tid ? tid : "");
    copy_trunc(b->st.version, sizeof(b->st.version), ver);
    if (err == TSC_OK && !tsc_valid_theme_id(tid)) err = TSC_ERR_THEME_ID;

    dbuf_t o;
    db_init(&o, &b->a, TSC_MAX_BYTES);
    if (err == TSC_OK) {
        dbuf_t head;
        db_init(&head, &b->a, TSC_MAX_BYTES);
        bool w = db_str(&head, "{\"format\":\"" TSC_FORMAT "\",\"theme_id\":") &&
                 db_jstr(&head, tid) && db_str(&head, ",\"project_version\":") &&
                 db_jstr(&head, ver) && db_str(&head, ",\"source\":");
        if (b->is_reduced && b->source.len) {
            w = w && db_put(&head, b->source.p, b->source.len);
        } else {
            char num[32];
            snprintf(num, sizeof(num), "%llu", (unsigned long long)src_size);
            w = w && db_str(&head, b->is_reduced ? "{\"kind\":\"sidecar\",\"size\":"
                                                 : "{\"kind\":\"gaugeproj\",\"size\":") &&
                db_str(&head, num);
            if (sha256_hex) w = w && db_str(&head, ",\"sha256\":") && db_jstr(&head, sha256_hex);
            w = w && db_str(&head, "}");
        }
        w = w && db_str(&head, ",\"asset_files\":{") &&
            db_put(&head, b->assets.p ? b->assets.p : "", b->assets.len) &&
            db_str(&head, "},\"pages\":[");
        if (head.nomem) err = TSC_ERR_NOMEM;
        else if (!w) err = TSC_ERR_TOO_BIG;

        if (err == TSC_OK) {
            // drop trailing pages until everything fits
            size_t pages_len = b->n_pages ? b->rec[b->n_pages - 1].end : 0;
            while (b->n_pages > 0 && head.len + pages_len + 2 > TSC_MAX_BYTES) {
                page_rec_t *r = &b->rec[--b->n_pages];
                b->st.pages--;
                b->st.pages_too_big++;
                b->st.rules = (uint16_t)(b->st.rules - r->rules);
                b->st.elements = (uint16_t)(b->st.elements - r->elements);
                b->st.overlays = (uint16_t)(b->st.overlays - r->overlays);
                pages_len = b->n_pages ? b->rec[b->n_pages - 1].end : 0;
            }
            if (head.len + pages_len + 2 > TSC_MAX_BYTES) {
                err = TSC_ERR_TOO_BIG;
            } else {
                w = db_put(&o, head.p, head.len) &&
                    db_put(&o, b->pages.p ? b->pages.p : "", pages_len) && db_str(&o, "]}");
                if (o.nomem) err = TSC_ERR_NOMEM;
                else if (!w) err = TSC_ERR_TOO_BIG;
            }
        }
        db_free(&head);
    }
    b->st.asset_files = b->n_assets;
    if (st) *st = b->st;
    if (err != TSC_OK) {
        db_free(&o);
        return err;
    }
    *out = o.p;
    if (out_len) *out_len = o.len;
    return TSC_OK;
}

void tsc_builder_free(tsc_builder_t *b)
{
    if (!b) return;
    db_free(&b->source);
    db_free(&b->assets);
    db_free(&b->pages);
    db_free(&b->ids);
    db_free(&b->ids_given_buf);
    db_free(&b->elems);
    db_free(&b->ovl);
    db_free(&b->rules);
    db_free(&b->page);
    a_free(&b->a, b->cap_buf);
    a_free(&b->a, b->item_buf);
    tsc_alloc_t a = b->a;
    a_free(&a, b);
}

// ---------------- reading a stored sidecar ----------------

typedef struct {
    tjs_t *nested;
    char *arena;
    size_t arena_cap;
    const char *want_id;
    uint32_t want_index;
    tsc_page_t *page;
    bool found;
} fp_ctx_t;

static bool fp_nested_cb(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                         const char *json, size_t len, bool overflow)
{
    (void)index;
    fp_ctx_t *f = ctx;
    if (kind != TJS_VALUE || overflow) return true;
    tsc_page_t *p = f->page;
    if (strcmp(key, "id") == 0) {
        cJSON *v = parse(json, len);
        if (cJSON_IsString(v) && strlen(v->valuestring) < sizeof(p->id)) {
            copy_trunc(p->id, sizeof(p->id), v->valuestring);
        }
        cJSON_Delete(v);
        return true;   // id text need not stay in the arena
    }
    if (strcmp(key, "element_ids") == 0 && json[0] == '[') {
        p->element_ids = json; p->element_ids_len = len;
    } else if (strcmp(key, "elements") == 0 && json[0] == '{') {
        p->elements = json; p->elements_len = len;
    } else if (strcmp(key, "overlay") == 0 && json[0] == '{') {
        p->overlay = json; p->overlay_len = len;
    } else if (strcmp(key, "rules") == 0 && json[0] == '[') {
        p->rules = json; p->rules_len = len;
    } else {
        return true;
    }
    // keep the captured piece, capture the next one after it
    size_t used = (size_t)(json - f->arena) + len + 1;
    if (used > f->arena_cap) used = f->arena_cap;
    tjs_set_buffer(f->nested, f->arena + used, f->arena_cap - used);
    return true;
}

static bool fp_top_cb(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                      const char *json, size_t len, bool overflow)
{
    (void)key;
    fp_ctx_t *f = ctx;
    if (kind != TJS_ITEM || overflow) return true;
    memset(f->page, 0, sizeof(*f->page));

    static const char *const cap[] = {"id", "element_ids", "elements", "overlay", "rules", NULL};
    tjs_t s;
    tjs_cfg_t cfg = {.capture_keys = cap, .buf = f->arena, .cap = f->arena_cap,
                     .cb = fp_nested_cb, .ctx = f};
    tjs_init(&s, &cfg);
    f->nested = &s;
    tjs_feed(&s, json, len);
    f->nested = NULL;
    if (!tjs_finish(&s) || !f->page->id[0]) return true;

    bool match = f->want_id ? strcmp(f->page->id, f->want_id) == 0 : index == f->want_index;
    if (!match) return true;
    f->found = true;
    return false;
}

bool tsc_find_page(const char *json, size_t len, const char *page_id, uint32_t index,
                   char *scratch, size_t scratch_cap, tsc_page_t *page)
{
    if (!json || !scratch || scratch_cap < 64 || !page) return false;
    memset(page, 0, sizeof(*page));
    size_t half = scratch_cap / 2;
    static const char *const split[] = {"pages", NULL};
    fp_ctx_t f = {
        .arena = scratch + half, .arena_cap = scratch_cap - half,
        .want_id = page_id, .want_index = index, .page = page,
    };
    tjs_cfg_t cfg = {.split_keys = split, .buf = scratch, .cap = half, .cb = fp_top_cb, .ctx = &f};
    tjs_run(&cfg, json, len);
    if (!f.found) memset(page, 0, sizeof(*page));
    return f.found;
}

typedef struct {
    char *tid; size_t tid_cap;
    char *ver; size_t ver_cap;
    uint16_t pages;
    bool fmt_ok, have_tid;
} hdr_ctx_t;

static bool hdr_cb(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                   const char *json, size_t len, bool overflow)
{
    (void)index;
    hdr_ctx_t *h = ctx;
    if (kind == TJS_ITEM) {
        if (h->pages < UINT16_MAX) h->pages++;
        return true;
    }
    if (overflow) return true;
    cJSON *v = parse(json, len);
    if (cJSON_IsString(v)) {
        if (strcmp(key, "format") == 0) {
            h->fmt_ok = strcmp(v->valuestring, TSC_FORMAT) == 0;
        } else if (strcmp(key, "theme_id") == 0 && strlen(v->valuestring) < 72) {
            copy_trunc(h->tid, h->tid_cap, v->valuestring);
            h->have_tid = true;
        } else if (strcmp(key, "project_version") == 0) {
            copy_trunc(h->ver, h->ver_cap, v->valuestring);
        }
    }
    cJSON_Delete(v);
    return true;
}

bool tsc_read_header(const char *json, size_t len, char *theme_id, size_t id_cap,
                     char *version, size_t ver_cap, uint16_t *n_pages)
{
    char tid[72] = "", ver[32] = "";
    char buf[256];
    static const char *const cap[] = {"format", "theme_id", "project_version", NULL};
    static const char *const split[] = {"pages", NULL};
    hdr_ctx_t h = {.tid = tid, .tid_cap = sizeof(tid), .ver = ver, .ver_cap = sizeof(ver)};
    tjs_cfg_t cfg = {.capture_keys = cap, .split_keys = split, .max_str = 200,
                     .buf = buf, .cap = sizeof(buf), .cb = hdr_cb, .ctx = &h};
    bool ok = json && tjs_run(&cfg, json, len) && h.fmt_ok && h.have_tid &&
              tsc_valid_theme_id(tid);
    if (theme_id) copy_trunc(theme_id, id_cap, ok ? tid : "");
    if (version) copy_trunc(version, ver_cap, ok ? ver : "");
    if (n_pages) *n_pages = ok ? h.pages : 0;
    return ok;
}

typedef struct {
    const char *asset;
    char *out;
    size_t cap;
    bool found;
} af_ctx_t;

static bool af_cb(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                  const char *json, size_t len, bool overflow)
{
    (void)kind; (void)key; (void)index;
    af_ctx_t *a = ctx;
    if (overflow) return false;
    cJSON *v = parse(json, len);
    const char *f = jstr(v, a->asset);
    if (f) {
        copy_trunc(a->out, a->cap, f);
        a->found = strlen(f) < a->cap;
    }
    cJSON_Delete(v);
    return false;
}

bool tsc_asset_file(const char *json, size_t len, const char *asset,
                    char *scratch, size_t scratch_cap, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!json || !asset || !scratch || !out || cap == 0) return false;
    static const char *const keys[] = {"asset_files", NULL};
    af_ctx_t a = {.asset = asset, .out = out, .cap = cap};
    tjs_cfg_t cfg = {.capture_keys = keys, .buf = scratch, .cap = scratch_cap,
                     .cb = af_cb, .ctx = &a};
    tjs_run(&cfg, json, len);
    if (!a.found) out[0] = '\0';
    return a.found;
}

// ---------------- pairing ----------------

static const tr_element_t *bin_find(const tr_element_t *bin, uint16_t n, const char *id)
{
    for (uint16_t i = 0; i < n; i++) {
        if (bin[i].id && strcmp(bin[i].id, id) == 0) return &bin[i];   // first wins
    }
    return NULL;
}

// Sidecar kind of `id` matches the theme.bin element kind.
static bool kind_match(const cJSON *elems, const tr_element_t *bin, uint16_t n, const char *id)
{
    if (!id) return false;
    const char *k = jstr(elems, id);
    const tr_element_t *e = bin_find(bin, n, id);
    return k && e && e->kind != TR_KIND_UNKNOWN && tr_kind_from_str(k) == e->kind;
}

void tsc_pair_page(const tsc_page_t *page, const tr_element_t *bin, uint16_t n_bin,
                   tsc_asset_exists_fn asset_exists, void *ctx, tsc_pair_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!page) return;

    cJSON *elems = parse(page->elements, page->elements_len);
    for (const cJSON *c = cJSON_IsObject(elems) ? elems->child : NULL; c; c = c->next) {
        if (!cJSON_IsString(c) || !c->string) continue;
        out->sc_elements++;
        const tr_element_t *e = bin_find(bin, n_bin, c->string);
        if (!e) out->missing++;
        else if (e->kind == TR_KIND_UNKNOWN || tr_kind_from_str(c->valuestring) != e->kind)
            out->kind_mismatch++;
        else out->matched++;
    }

    cJSON *ids = parse(page->element_ids, page->element_ids_len);
    if (cJSON_IsArray(ids)) {
        bool same = (int)n_bin == cJSON_GetArraySize(ids);
        uint16_t i = 0;
        for (const cJSON *c = ids->child; same && c; c = c->next, i++) {
            same = cJSON_IsString(c) && bin[i].id && strcmp(c->valuestring, bin[i].id) == 0;
        }
        out->in_sync = same;
    }
    cJSON_Delete(ids);

    cJSON *ovl = parse(page->overlay, page->overlay_len);
    for (const cJSON *c = cJSON_IsObject(ovl) ? ovl->child : NULL; c; c = c->next) {
        out->overlays++;
        if (!kind_match(elems, bin, n_bin, c->string)) out->overlays_unmatched++;
    }
    cJSON_Delete(ovl);

    cJSON *rules = parse(page->rules, page->rules_len);
    uint16_t n_rules = 0;
    for (const cJSON *r = cJSON_IsArray(rules) ? rules->child : NULL; r; r = r->next) {
        if (!cJSON_IsObject(r)) continue;
        if (n_rules >= TR_MAX_RULES) break;
        n_rules++;
        const cJSON *when = jget(r, "when");
        const cJSON *conds = jget(when, "conditions");
        if (cJSON_IsArray(conds) && cJSON_GetArraySize(conds) > TR_MAX_CONDS) {
            out->conds_truncated = (uint16_t)(out->conds_truncated +
                                              (cJSON_GetArraySize(conds) - TR_MAX_CONDS));
        }
        const cJSON *apply = jget(r, "apply");
        int ai = 0;
        for (const cJSON *a = cJSON_IsArray(apply) ? apply->child : NULL; a; a = a->next, ai++) {
            if (ai >= TR_MAX_ACTIONS) {
                out->actions_truncated++;
                continue;
            }
            out->actions++;
            if (!kind_match(elems, bin, n_bin, jstr(a, "target"))) {
                out->actions_no_target++;
                continue;
            }
            const char *asset = jstr(jget(a, "set"), "asset");
            if (asset && asset_exists && !asset_exists(asset, ctx)) out->actions_no_asset++;
        }
    }
    out->rules = n_rules;
    cJSON_Delete(rules);
    cJSON_Delete(elems);
}
