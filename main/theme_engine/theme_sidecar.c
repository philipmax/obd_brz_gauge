// ============================================================
//  theme_sidecar.c — sidecar install / pairing report / storage
//  (contracts/theme-editor-contract.md B2/B3, REFACTOR_PLAN.md P4).
//
//  Stored form: the reduced "gauge-sidecar/1" document (<= 64 KB) at
//  SD /OBD/THEMES/<storage_theme_key(theme_id)>/sidecar.json. The current
//  custom theme keeps its sidecar in PSRAM (g_te->sc_json); pages compile rules and overlays from it at
//  build time and never point into it afterwards.
//
//  Called from httpd / storage tasks: no LVGL calls here. SD writes run
//  outside the engine lock; pairing runs under it.
// ============================================================

#include "theme_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "bsp_obd_dsp/config_part.h"
#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/storage_util.h"

#define TE_SC_CHUNK     8192u
#define TE_SC_PATH_MAX  160

// ---------------- report buffer ----------------

typedef struct {
    char  *buf;
    size_t cap, len;
    bool   full;
} te_rep_t;

static void rep_init(te_rep_t *r, char *buf, size_t cap)
{
    r->buf = buf;
    r->cap = buf ? cap : 0;
    r->len = 0;
    r->full = false;
    if (r->cap) buf[0] = '\0';
}

static void rep_add(te_rep_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rep_add(te_rep_t *r, const char *fmt, ...)
{
    if (!r->cap || r->full) return;
    size_t room = r->cap - r->len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->buf + r->len, room, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < room) {
        r->len += (size_t)n;
        return;
    }
    // truncated: end with "..."
    r->full = true;
    r->len = r->cap - 1;
    if (r->cap >= 4) memcpy(r->buf + r->cap - 4, "...", 4);
}

// ---------------- paths ----------------

static void te_sc_path(const char *theme_id, char *out, size_t cap)
{
    char key[STORAGE_THEME_KEY_LEN];
    storage_theme_key(theme_id, key, sizeof(key));
    snprintf(out, cap, SD_DIR_THEMES "/%s/sidecar.json", key);
}

static void te_sc_legacy_path(const char *theme_id, char *out, size_t cap)
{
    char key[STORAGE_THEME_KEY_LEN];
    storage_theme_key(theme_id, key, sizeof(key));
    snprintf(out, cap, CONFIG_MOUNT_POINT "/themes/%s/sidecar.json", key);
}

// ---------------- cache ----------------

void te_sidecar_drop(void)
{
    te_ctx_t *c = g_te;
    if (!c) return;
    te_free(c->sc_json);
    c->sc_json = NULL;
    c->sc_len = 0;
}

void te_sidecar_reload(void)
{
    te_ctx_t *c = g_te;
    te_sidecar_drop();
    if (!c || !c->custom || !tsc_valid_theme_id(c->id)) return;

    char path[TE_SC_PATH_MAX];
    te_sc_path(c->id, path, sizeof(path));
    char *buf = te_malloc(TSC_MAX_BYTES + 1);
    if (!buf) {
        ESP_LOGE(TE_TAG, "sidecar: no memory");
        return;
    }
    size_t n = 0;
    bool legacy = false;
    esp_err_t err = sd_card_read_file(path, buf, TSC_MAX_BYTES + 1, &n);
    if (err == ESP_ERR_NOT_FOUND && sd_card_is_mounted() && config_part_is_mounted()) {
        char old_path[TE_SC_PATH_MAX];
        te_sc_legacy_path(c->id, old_path, sizeof(old_path));
        err = config_part_read(old_path, buf, TSC_MAX_BYTES + 1, &n);
        legacy = err == ESP_OK;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TE_TAG, "no sidecar for '%s' (bin-only install: no rules / overlay)", c->id);
        te_free(buf);
        return;
    }
    if (err == ESP_OK && (n == 0 || n > TSC_MAX_BYTES)) err = ESP_ERR_INVALID_SIZE;
    if (err != ESP_OK) {
        ESP_LOGW(TE_TAG, "sidecar %s: %s, ignored", path, esp_err_to_name(err));
        te_free(buf);
        return;
    }
    buf[n] = '\0';
    char tid[72];
    if (!tsc_read_header(buf, n, tid, sizeof(tid), NULL, 0, NULL) || strcmp(tid, c->id) != 0) {
        ESP_LOGW(TE_TAG, "sidecar %s: bad header or theme_id mismatch, ignored", path);
        te_free(buf);
        return;
    }
    if (legacy) {
        storage_sd_mkdir_parents(path);
        if (sd_card_write_file_atomic(path, buf, n) == ESP_OK) {
            char old_path[TE_SC_PATH_MAX];
            te_sc_legacy_path(c->id, old_path, sizeof(old_path));
            config_part_remove(old_path);
            ESP_LOGI(TE_TAG, "migrated legacy sidecar for '%s' to SD", c->id);
        }
    }
    char *s = heap_caps_realloc(buf, n + 1, TE_CAPS);
    c->sc_json = s ? s : buf;
    c->sc_len = n;
    ESP_LOGI(TE_TAG, "sidecar paired with '%s' (%u bytes)", c->id, (unsigned)n);
}

// ---------------- pairing report ----------------

typedef struct {
    te_ctx_t *c;
    char *scratch;            // TSC_ITEM_MAX, for asset_files lookups
} te_asset_ctx_t;

static bool te_asset_exists(const char *name, void *ctx)
{
    te_asset_ctx_t *a = ctx;
    if (te_image_find(name) >= 0) return true;
    if (!a->scratch || !a->c->sc_json) return false;
    char file[96];
    return tsc_asset_file(a->c->sc_json, a->c->sc_len, name, a->scratch, TSC_ITEM_MAX, file,
                          sizeof(file));
}

typedef struct {
    te_rep_t *r;
    uint16_t n;
} te_skip_ctx_t;

static void te_report_skip(const char *id, size_t cost, void *ctx)
{
    te_skip_ctx_t *s = ctx;
    if (s->n < TE_REPORT_SKIP_MAX) {
        rep_add(s->r, "%s%s (%uK)", s->n ? ", " : "; est. skipped: ", id[0] ? id : "?",
                (unsigned)((cost + 1023) / 1024));
    } else if (s->n == TE_REPORT_SKIP_MAX) {
        rep_add(s->r, ", ...");
    }
    s->n++;
}

static void te_report_page(te_ctx_t *c, int bin_idx, te_asset_ctx_t *ac, te_rep_t *r)
{
    te_prep_t prep;
    esp_err_t err = te_prep_page(bin_idx, true, &prep);
    if (err != ESP_OK || !prep.layout) {
        rep_add(r, "page %s: layout unreadable (%s)\n", c->pages[bin_idx].id, esp_err_to_name(err));
        te_prep_free(&prep);
        return;
    }
    const tl_layout_t *l = prep.layout;
    tr_element_t *bin = te_calloc(l->n, sizeof(tr_element_t));
    if (!bin) {
        rep_add(r, "page %s: no memory\n", c->pages[bin_idx].id);
        te_prep_free(&prep);
        return;
    }
    for (uint16_t i = 0; i < l->n; i++) {
        bin[i].id = l->el[i].id;
        bin[i].kind = (tr_kind_t)l->el[i].kind;
    }
    tsc_pair_t pr;
    tsc_pair_page(prep.sc, bin, l->n, te_asset_exists, ac, &pr);
    te_free(bin);

    rep_add(r, "page %s: %u/%u matched", c->pages[bin_idx].id, pr.matched, pr.sc_elements);
    if (pr.missing) rep_add(r, ", %u missing", pr.missing);
    if (pr.kind_mismatch) rep_add(r, ", %u kind mismatch", pr.kind_mismatch);
    if (!pr.in_sync) rep_add(r, ", out of sync");
    rep_add(r, "; %u rules", pr.rules);
    if (pr.actions_no_target) rep_add(r, ", %u actions without target", pr.actions_no_target);
    if (pr.actions_no_asset) rep_add(r, ", %u unknown asset", pr.actions_no_asset);
    if (pr.actions_truncated || pr.conds_truncated) {
        rep_add(r, ", truncated %u actions %u conds", pr.actions_truncated, pr.conds_truncated);
    }
    if (prep.diag.unknown_sources) rep_add(r, ", %u unknown sources", prep.diag.unknown_sources);
    if (pr.overlays) {
        rep_add(r, "; %u overlays", pr.overlays);
        if (pr.overlays_unmatched) rep_add(r, " (%u unmatched)", pr.overlays_unmatched);
    }
    if (l->unknown_types) rep_add(r, "; %u unknown elements", l->unknown_types);
    te_skip_ctx_t sk = {r, 0};
    te_estimate_page(&prep, te_report_skip, &sk);
    rep_add(r, "\n");
    te_prep_free(&prep);
}

// Caller holds te_lock; c->sc_json is the sidecar of the current theme.
static void te_report_paired(te_ctx_t *c, const tsc_stats_t *st, te_rep_t *r)
{
    rep_add(r, "theme %s v%s: %u pages, %u rules", st->theme_id, st->version, st->pages,
            st->rules);
    if (st->rules_truncated) rep_add(r, ", %u rules truncated", st->rules_truncated);
    if (st->pages_over_limit || st->pages_too_big || st->pages_invalid) {
        rep_add(r, ", pages dropped: %u over limit %u too big %u invalid", st->pages_over_limit,
                st->pages_too_big, st->pages_invalid);
    }
    if (st->elements_truncated) rep_add(r, ", %u element ids truncated", st->elements_truncated);
    rep_add(r, "\n");

    uint16_t n_pages = 0;
    tsc_read_header(c->sc_json, c->sc_len, NULL, 0, NULL, 0, &n_pages);
    size_t cap = 2 * (c->sc_len + 64);
    char *scratch = te_malloc(cap);
    te_asset_ctx_t ac = {c, te_malloc(TSC_ITEM_MAX)};
    tsc_page_t *pg = te_malloc(sizeof(*pg));
    if (!scratch || !ac.scratch || !pg) {
        rep_add(r, "pairing: no memory\n");
        goto out;
    }
    uint16_t missing = 0;
    for (uint16_t i = 0; i < n_pages && i < TSC_MAX_PAGES; i++) {
        if (!tsc_find_page(c->sc_json, c->sc_len, NULL, i, scratch, cap, pg)) continue;
        int bin_idx = te_page_find(pg->id);
        if (bin_idx < 0) {
            rep_add(r, "%s%s", missing++ ? ", " : "not in theme.bin: ", pg->id);
            continue;
        }
        if (missing) {
            rep_add(r, "\n");
            missing = 0;
        }
        te_report_page(c, bin_idx, &ac, r);
    }
    if (missing) rep_add(r, "\n");

out:
    te_free(pg);
    te_free(ac.scratch);
    te_free(scratch);
}

// ---------------- install ----------------

static esp_err_t te_sc_err(tsc_err_t e, te_rep_t *r)
{
    switch (e) {
    case TSC_ERR_JSON:
        rep_add(r, "invalid sidecar: not a JSON object or truncated");
        return ESP_ERR_INVALID_ARG;
    case TSC_ERR_FORMAT:
        rep_add(r, "invalid sidecar: format is not " TSC_FORMAT);
        return ESP_ERR_INVALID_ARG;
    case TSC_ERR_THEME_ID:
        rep_add(r, "invalid sidecar: missing or invalid theme_id / meta.id");
        return ESP_ERR_INVALID_ARG;
    case TSC_ERR_TOO_BIG:
        rep_add(r, "sidecar too big: reduced form exceeds %u KB", (unsigned)(TSC_MAX_BYTES / 1024));
        return ESP_ERR_INVALID_SIZE;
    case TSC_ERR_NOMEM:
        rep_add(r, "out of memory");
        return ESP_ERR_NO_MEM;
    default:
        return ESP_OK;
    }
}

typedef struct {
    tsc_builder_t *b;
    mbedtls_sha256_context sha;
    uint64_t size;
    bool ok;
} te_feed_t;

static bool te_feed(const uint8_t *data, size_t len, void *ctx)
{
    te_feed_t *f = ctx;
    mbedtls_sha256_update(&f->sha, data, len);
    f->size += len;
    if (!tsc_builder_feed(f->b, (const char *)data, len)) f->ok = false;
    return f->ok;
}

static esp_err_t te_feed_begin(te_feed_t *f)
{
    memset(f, 0, sizeof(*f));
    tsc_alloc_t a = TE_ALLOC_INIT;
    f->b = tsc_builder_new(&a);
    if (!f->b) return ESP_ERR_NO_MEM;
    mbedtls_sha256_init(&f->sha);
    mbedtls_sha256_starts(&f->sha, 0);
    f->ok = true;
    return ESP_OK;
}

// Finish the builder, store the reduced sidecar and pair it when it
// belongs to the current theme. Consumes f.
static esp_err_t te_feed_commit(te_ctx_t *c, te_feed_t *f, te_rep_t *r)
{
    unsigned char dig[32];
    char hex[65];
    mbedtls_sha256_finish(&f->sha, dig);
    mbedtls_sha256_free(&f->sha);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", dig[i]);

    char *out = NULL;
    size_t out_len = 0;
    tsc_stats_t st;
    memset(&st, 0, sizeof(st));
    tsc_err_t e = tsc_builder_finish(f->b, f->size, hex, &out, &out_len, &st);
    tsc_builder_free(f->b);
    f->b = NULL;
    if (e != TSC_OK) {
        te_free(out);
        return te_sc_err(e, r);
    }

    char path[TE_SC_PATH_MAX];
    te_sc_path(st.theme_id, path, sizeof(path));
    if (!sd_card_is_mounted()) {
        rep_add(r, "SD card required to store theme sidecars");
        te_free(out);
        return ESP_ERR_INVALID_STATE;
    }
    storage_sd_mkdir_parents(path);
    esp_err_t err = sd_card_write_file_atomic(path, out, out_len);
    if (err != ESP_OK) {
        rep_add(r, "store %s failed: %s", path, esp_err_to_name(err));
        te_free(out);
        return err;
    }

    te_lock();
    if (c->custom && strcmp(c->id, st.theme_id) == 0) {
        te_free(c->sc_json);
        c->sc_json = out;
        c->sc_len = out_len;
        out = NULL;
        te_report_paired(c, &st, r);
        rep_add(r, "applies on the next page build; bin-only installs (app/store/web flash) "
                   "have no overlay");
    } else {
        rep_add(r, "theme %s v%s: %u pages, %u rules stored; applies when that theme is "
                   "installed (current: %s)",
                st.theme_id, st.version, st.pages, st.rules,
                c->custom ? c->id : "built-in");
    }
    te_unlock();
    te_free(out);
    ESP_LOGI(TE_TAG, "sidecar for '%s' stored (%u bytes from %llu)", st.theme_id,
             (unsigned)out_len, (unsigned long long)f->size);
    return ESP_OK;
}

static void te_feed_abort(te_feed_t *f)
{
    if (!f->b) return;
    mbedtls_sha256_free(&f->sha);
    tsc_builder_free(f->b);
    f->b = NULL;
}

esp_err_t theme_sidecar_install(const char *json, size_t len, char *report, size_t report_len)
{
    te_rep_t r;
    rep_init(&r, report, report_len);
    if (!json || len == 0) {
        rep_add(&r, "invalid sidecar: empty");
        return ESP_ERR_INVALID_ARG;
    }
    te_ctx_t *c = te_ctx_ensure();
    if (!c) return ESP_ERR_NO_MEM;
    te_feed_t f;
    esp_err_t err = te_feed_begin(&f);
    if (err != ESP_OK) {
        rep_add(&r, "out of memory");
        return err;
    }
    te_feed((const uint8_t *)json, len, &f);
    return te_feed_commit(c, &f, &r);
}

esp_err_t theme_sidecar_install_file(const char *path, char *report, size_t report_len)
{
    te_rep_t r;
    rep_init(&r, report, report_len);
    if (!path || !path[0]) return ESP_ERR_INVALID_ARG;
    te_ctx_t *c = te_ctx_ensure();
    if (!c) return ESP_ERR_NO_MEM;
    te_feed_t f;
    esp_err_t err = te_feed_begin(&f);
    if (err != ESP_OK) {
        rep_add(&r, "out of memory");
        return err;
    }

    size_t mlen = strlen(SD_MOUNT_POINT);
    if (strncmp(path, SD_MOUNT_POINT, mlen) == 0 && path[mlen] == '/') {
        // SD: chunks come through the shared DMA bounce buffer
        err = storage_sd_file_foreach(path, 0, 0, te_feed, &f);
    } else {
        FILE *fp = fopen(path, "rb");
        uint8_t *chunk = fp ? te_malloc(TE_SC_CHUNK) : NULL;
        if (!fp) err = ESP_ERR_NOT_FOUND;
        else if (!chunk) err = ESP_ERR_NO_MEM;
        else {
            size_t n;
            while (f.ok && (n = fread(chunk, 1, TE_SC_CHUNK, fp)) > 0) te_feed(chunk, n, &f);
            if (ferror(fp)) err = ESP_FAIL;
        }
        te_free(chunk);
        if (fp) fclose(fp);
    }
    if (err != ESP_OK) {
        rep_add(&r, "read %s failed: %s", path, esp_err_to_name(err));
        te_feed_abort(&f);
        return err;
    }
    return te_feed_commit(c, &f, &r);
}

esp_err_t theme_sidecar_remove(const char *theme_id)
{
    te_ctx_t *c = te_ctx_ensure();
    if (!c) return ESP_ERR_NO_MEM;
    char id[TE_ID_CAP];
    if (!theme_id) {
        te_lock();
        bool custom = c->custom;
        if (custom) memcpy(id, c->id, sizeof(id));
        te_unlock();
        if (!custom) return ESP_ERR_INVALID_STATE;
    } else {
        if (strlen(theme_id) >= sizeof(id)) return ESP_ERR_INVALID_ARG;
        strcpy(id, theme_id);
    }
    if (!tsc_valid_theme_id(id)) return ESP_ERR_INVALID_ARG;

    char path[TE_SC_PATH_MAX];
    te_sc_path(id, path, sizeof(path));
    esp_err_t err = sd_card_remove(path);
    if (err == ESP_OK) {
        char old_path[TE_SC_PATH_MAX];
        te_sc_legacy_path(id, old_path, sizeof(old_path));
        if (config_part_is_mounted()) config_part_remove(old_path);
        te_lock();
        if (c->custom && strcmp(c->id, id) == 0) te_sidecar_drop();
        te_unlock();
        ESP_LOGI(TE_TAG, "sidecar for '%s' removed", id);
    }
    return err;
}

bool theme_sidecar_present(void)
{
    te_ctx_t *c = g_te;
    if (!c) return false;
    te_lock();
    bool p = c->custom && c->sc_json != NULL;
    te_unlock();
    return p;
}
