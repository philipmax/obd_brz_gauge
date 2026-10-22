// ============================================================
//  theme_loader.c — SD manifest, asset tables, fonts and
//  the theme lifecycle (contracts/theme-editor-contract.md A1-A3).
//
//  theme.bin = [0,16 KB) manifest JSON (0xFF padded, no NUL), then
//  images, fonts and page layouts at the absolute offsets the manifest
//  lists. Only [0, used_len) is valid: nothing is ever assumed about
//  the rest of the file.
//
//  Everything the loader owns lives in PSRAM; the only static storage
//  is g_te (one pointer).
// ============================================================

#include "theme_internal.h"
#include "app_obd_dsp/theme_library.h"
#include "app_obd_dsp/storage_util.h"
#include "bsp_obd_dsp/sd_card.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "cJSON.h"
#include "rules/tr_parse.h"

te_ctx_t *g_te;

#define TE_FONT_LETTER  'F'

static const char *const k_color_keys[UI_COLOR__COUNT] = {
    "bg", "ring", "arc_track", "arc_indicator",
    "text_primary", "text_secondary", "needle", "panel",
};

// ---------------- allocation / lock ----------------

char *te_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = te_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void *te_alloc_cb(size_t size, void *ctx)
{
    (void)ctx;
    return te_malloc(size);
}

void te_free_cb(void *ptr, void *ctx)
{
    (void)ctx;
    if (ptr) te_free(ptr);
}

void *te_calloc_cb(size_t n, size_t size, void *ctx)
{
    (void)ctx;
    return te_calloc(n, size);
}

te_ctx_t *te_ctx_ensure(void)
{
    te_ctx_t *c = __atomic_load_n(&g_te, __ATOMIC_ACQUIRE);
    if (c) return c;
    te_ctx_t *n = te_calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->lock = xSemaphoreCreateRecursiveMutex();
    if (!n->lock) {
        te_free(n);
        return NULL;
    }
    tic_init(&n->cache);
    te_ctx_t *expected = NULL;
    if (!__atomic_compare_exchange_n(&g_te, &expected, n, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        vSemaphoreDelete(n->lock);
        te_free(n);
        return expected;
    }
    return n;
}

void te_lock(void)
{
    xSemaphoreTakeRecursive(g_te->lock, portMAX_DELAY);
}

void te_unlock(void)
{
    xSemaphoreGiveRecursive(g_te->lock);
}

bool te_trylock(void)
{
    return xSemaphoreTakeRecursive(g_te->lock, 0) == pdTRUE;
}

// ---------------- small helpers ----------------

// Copy with truncation on a UTF-8 character boundary.
static void te_copy_utf8(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) n--;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *te_jstr(const cJSON *o, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

// Integral, non-negative number that fits in uint32_t.
static bool te_ju32(const cJSON *o, const char *key, uint32_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(it)) return false;
    double v = it->valuedouble;
    if (!(v >= 0.0) || v > 4294967295.0 || floor(v) != v) return false;
    *out = (uint32_t)v;
    return true;
}

static void te_tables_free(te_ctx_t *c)
{
    for (uint8_t i = 0; i < c->n_images; i++) te_free(c->images[i].name);
    for (uint8_t i = 0; i < c->n_fonts; i++) te_free(c->fonts[i].name);
    for (uint8_t i = 0; i < c->n_pages; i++) te_free(c->pages[i].id);
    memset(c->images, 0, sizeof(c->images));
    memset(c->fonts, 0, sizeof(c->fonts));
    memset(c->pages, 0, sizeof(c->pages));
    c->n_images = c->n_fonts = c->n_pages = 0;
}

static bool te_name_taken_img(const te_ctx_t *c, const char *name)
{
    for (uint8_t i = 0; i < c->n_images; i++) {
        if (strcmp(c->images[i].name, name) == 0) return true;
    }
    return false;
}

static bool te_name_taken_font(const te_ctx_t *c, const char *name)
{
    for (uint8_t i = 0; i < c->n_fonts; i++) {
        if (strcmp(c->fonts[i].name, name) == 0) return true;
    }
    return false;
}

// ---------------- manifest ----------------

// One "assets" entry. Returns the end offset (0 = skipped).
static uint32_t te_add_asset(te_ctx_t *c, const cJSON *a, bool *nomem)
{
    const char *name = a->string;
    const char *fmt = te_jstr(a, "format");
    uint32_t off = 0, size = 0;
    if (!cJSON_IsObject(a) || !name || !name[0]) return 0;
    if (!te_ju32(a, "offset", &off) || !te_ju32(a, "size", &size) || size == 0 ||
        off < TE_MANIFEST_SIZE || (uint64_t)off + size > c->file_size) {
        ESP_LOGW(TE_TAG, "asset '%s': bad offset/size, skipped", name);
        return 0;
    }
    if (fmt && strcmp(fmt, "lv_font_bin") == 0) {
        if (te_name_taken_font(c, name)) return 0;   // duplicate: first wins
        if (c->n_fonts >= TE_MAX_FONTS) {
            ESP_LOGW(TE_TAG, "font '%s': more than %d fonts, skipped", name, TE_MAX_FONTS);
            return 0;
        }
        te_font_t *f = &c->fonts[c->n_fonts];
        f->name = te_strdup(name);
        if (!f->name) {
            *nomem = true;
            return 0;
        }
        f->offset = off;
        f->size = size;
        c->n_fonts++;
        return off + size;
    }
    tpx_fmt_t pf;
    if (fmt && strcmp(fmt, "rgb565") == 0) pf = TPX_FMT_RGB565;
    else if (fmt && strcmp(fmt, "rgba8888") == 0) pf = TPX_FMT_RGBA8888;
    else {
        ESP_LOGW(TE_TAG, "asset '%s': unknown format '%s', skipped", name, fmt ? fmt : "(none)");
        return 0;
    }
    uint32_t w = 0, h = 0;
    if (!te_ju32(a, "width", &w) || !te_ju32(a, "height", &h) || !tpx_validate(pf, w, h, size)) {
        ESP_LOGW(TE_TAG, "image '%s': size/width/height mismatch or > %d px, skipped", name,
                 TPX_MAX_DIM);
        return 0;
    }
    if (te_name_taken_img(c, name)) return 0;
    if (c->n_images >= TE_MAX_IMAGES) {
        ESP_LOGW(TE_TAG, "image '%s': more than %d images, skipped", name, TE_MAX_IMAGES);
        return 0;
    }
    te_image_t *im = &c->images[c->n_images];
    im->name = te_strdup(name);
    if (!im->name) {
        *nomem = true;
        return 0;
    }
    im->offset = off;
    im->size = size;
    im->w = (uint16_t)w;
    im->h = (uint16_t)h;
    im->fmt = (uint8_t)pf;
    c->n_images++;
    return off + size;
}

static uint32_t te_add_page(te_ctx_t *c, const cJSON *p, bool *nomem)
{
    const char *id = te_jstr(p, "id");
    uint32_t off = 0, size = 0;
    if (!cJSON_IsObject(p) || !id || !id[0]) return 0;
    if (strcmp(id, "logo") == 0 || strcmp(id, "intro") == 0 || strcmp(id, "boot_video") == 0) {
        ESP_LOGW(TE_TAG, "page '%s': boot pages are never themed, skipped", id);
        return 0;
    }
    if (te_page_find(id) >= 0) return 0;   // duplicate: first wins
    if (!te_ju32(p, "layout_data_offset", &off) || !te_ju32(p, "layout_data_size", &size) ||
        size == 0 || size > TE_LAYOUT_MAX || off < TE_MANIFEST_SIZE ||
        (uint64_t)off + size > c->file_size) {
        ESP_LOGW(TE_TAG, "page '%s': bad layout offset/size, skipped", id);
        return 0;
    }
    if (c->n_pages >= TE_MAX_PAGES) {
        ESP_LOGW(TE_TAG, "page '%s': more than %d pages, skipped", id, TE_MAX_PAGES);
        return 0;
    }
    te_page_t *pg = &c->pages[c->n_pages];
    pg->id = te_strdup(id);
    if (!pg->id) {
        *nomem = true;
        return 0;
    }
    pg->offset = off;
    pg->size = size;
    c->n_pages++;
    return off + size;
}

static esp_err_t te_parse_manifest(te_ctx_t *c, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TE_TAG, "manifest: not a JSON object");
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    const char *schema = te_jstr(root, "schema_version");
    const cJSON *theme = cJSON_GetObjectItemCaseSensitive(root, "theme");
    const char *id = te_jstr(theme, "id");
    if (!schema || strcmp(schema, "1.0") != 0) {
        ESP_LOGE(TE_TAG, "manifest: schema_version '%s' not supported (need \"1.0\")",
                 schema ? schema : "(none)");
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    if (!id || !id[0] || strlen(id) > THEME_ID_LEN) {
        ESP_LOGE(TE_TAG, "manifest: theme.id missing or longer than %d", THEME_ID_LEN);
        err = ESP_FAIL;
        goto out;
    }

    memcpy(c->id, id, strlen(id) + 1);
    memset(&c->info, 0, sizeof(c->info));
    te_copy_utf8(c->info.id, sizeof(c->info.id), id);
    const char *name = te_jstr(theme, "name");
    te_copy_utf8(c->info.name, sizeof(c->info.name), (name && name[0]) ? name : id);
    te_copy_utf8(c->info.version, sizeof(c->info.version), te_jstr(theme, "version"));
    te_copy_utf8(c->info.author, sizeof(c->info.author), te_jstr(theme, "author"));

    const ui_theme_t *def = ui_theme_get(0);
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(root, "colors");
    for (int i = 0; i < UI_COLOR__COUNT; i++) {
        uint32_t rgb;
        const char *s = te_jstr(colors, k_color_keys[i]);
        c->colors[i] = (s && tr_parse_color(s, &rgb)) ? rgb : (def ? def->colors[i] : 0);
    }

    bool nomem = false;
    uint32_t used = TE_MANIFEST_SIZE;
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    for (const cJSON *a = cJSON_IsObject(assets) ? assets->child : NULL; a && !nomem; a = a->next) {
        uint32_t end = te_add_asset(c, a, &nomem);
        if (end > used) used = end;
    }
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "pages"), "theme_pages");
    for (const cJSON *p = cJSON_IsArray(pages) ? pages->child : NULL; p && !nomem; p = p->next) {
        uint32_t end = te_add_page(c, p, &nomem);
        if (end > used) used = end;
    }
    if (nomem) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    c->used_len = used;

out:
    cJSON_Delete(root);
    if (err != ESP_OK) te_tables_free(c);
    return err;
}

static esp_err_t te_load_custom(te_ctx_t *c)
{
    char key[STORAGE_THEME_KEY_LEN];
    theme_library_current_key(key, sizeof(key));
    if (!key[0] || !sd_card_is_mounted()) return ESP_ERR_NOT_FOUND;
    if (strchr(key, '/') || strchr(key, '\\') || !strcmp(key, ".") || !strcmp(key, ".."))
        return ESP_ERR_INVALID_ARG;
    snprintf(c->sd_path, sizeof(c->sd_path), SD_DIR_THEMES "/%s/" STORAGE_THEME_FILE, key);
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    storage_theme_meta_t meta;
    esp_err_t err = storage_theme_meta_from_file(c->sd_path, &meta);
    int64_t size = storage_sd_file_size(c->sd_path);
    if (err != ESP_OK || size < TE_MANIFEST_SIZE || size > UINT32_MAX) {
        storage_job_unlock();
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }
    c->file_size = (uint32_t)size;
    uint8_t *buf = te_malloc(TE_MANIFEST_SIZE);
    if (!buf) { storage_job_unlock(); return ESP_ERR_NO_MEM; }
    err = te_read(0, buf, TE_MANIFEST_SIZE);
    if (err == ESP_OK) {
        size_t len = 0;
        while (len < TE_MANIFEST_SIZE && buf[len] != 0xFF && buf[len] != 0) len++;
        err = te_parse_manifest(c, (const char *)buf, len);
        if (err == ESP_OK) c->manifest_crc = esp_rom_crc32_le(0, buf, TE_MANIFEST_SIZE);
    }
    te_free(buf);
    storage_job_unlock();
    return err;
}

static void te_set_default(te_ctx_t *c)
{
    te_tables_free(c);
    c->part = NULL;
    c->sd_path[0] = 0;
    c->file_size = 0;
    c->used_len = 0;
    c->custom = false;
    c->manifest_crc = 0;
    memset(&c->info, 0, sizeof(c->info));
    strcpy(c->info.id, "default");
    strcpy(c->info.name, "DEFAULT");
    strcpy(c->info.version, "1.0.0");
    strcpy(c->info.author, "builtin");
    strcpy(c->id, "default");
    for (int i = 0; i < UI_COLOR__COUNT; i++) c->colors[i] = ui_theme_color((ui_color_role_t)i);
    c->loaded = true;
}

// ---------------- SD reads / lookups ----------------

typedef struct { uint8_t *next; } te_read_ctx_t;
static bool te_read_chunk(const uint8_t *data, size_t size, void *ctx)
{
    te_read_ctx_t *r = ctx;
    memcpy(r->next, data, size);
    r->next += size;
    return true;
}

esp_err_t te_read(uint32_t offset, void *dst, uint32_t size)
{
    return te_read_from(g_te, offset, dst, size);
}

esp_err_t te_read_from(te_ctx_t *c, uint32_t offset, void *dst, uint32_t size)
{
    if (!c || !dst || !c->sd_path[0] || !sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if ((uint64_t)offset + size > c->file_size) return ESP_ERR_INVALID_SIZE;
    if (!size) return ESP_OK;
    /* Bounded wait also avoids inversion with storage workers that acquire
     * the theme lock while installing a sidecar. No open FAT file survives. */
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    te_read_ctx_t r = {dst};
    esp_err_t err = storage_sd_file_foreach(c->sd_path, offset, size, te_read_chunk, &r);
    storage_job_unlock();
    return err;
}

int te_image_find(const char *name)
{
    te_ctx_t *c = g_te;
    if (!c || !name) return -1;
    for (uint8_t i = 0; i < c->n_images; i++) {
        if (strcmp(c->images[i].name, name) == 0) return i;
    }
    return -1;
}

int te_page_find(const char *id)
{
    te_ctx_t *c = g_te;
    if (!c || !id) return -1;
    for (uint8_t i = 0; i < c->n_pages; i++) {
        if (strcmp(c->pages[i].id, id) == 0) return i;
    }
    return -1;
}

tl_defaults_t te_layout_defaults(void)
{
    const te_ctx_t *c = g_te;
    tl_defaults_t d = {
        c->colors[UI_COLOR_BG], c->colors[UI_COLOR_ARC_TRACK],
        c->colors[UI_COLOR_ARC_INDICATOR], c->colors[UI_COLOR_TEXT_PRIMARY],
    };
    return d;
}

// ---------------- 'F' font driver (temporary SD segment during lv_font_load) ----------------

typedef struct {
    uint8_t *data;
    uint32_t size, pos;
} te_font_file_t;

static void *te_fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    te_ctx_t *c = drv->user_data;
    if (mode != LV_FS_MODE_RD || !c || !c->sd_path[0] || !path) return NULL;
    for (uint8_t i = 0; i < c->n_fonts; i++) {
        if (strcmp(c->fonts[i].name, path) != 0) continue;
        te_font_file_t *f = te_malloc(sizeof(*f));
        if (!f) return NULL;
        f->data = te_malloc(c->fonts[i].size);
        if (!f->data || te_read(c->fonts[i].offset, f->data, c->fonts[i].size) != ESP_OK) {
            te_free(f->data);
            te_free(f);
            return NULL;
        }
        f->size = c->fonts[i].size;
        f->pos = 0;
        return f;
    }
    return NULL;
}

static lv_fs_res_t te_fs_close(lv_fs_drv_t *drv, void *file)
{
    (void)drv;
    te_free(((te_font_file_t *)file)->data);
    te_free(file);
    return LV_FS_RES_OK;
}

static lv_fs_res_t te_fs_read(lv_fs_drv_t *drv, void *file, void *buf, uint32_t btr, uint32_t *br)
{
    (void)drv;
    te_font_file_t *f = file;
    uint32_t n = f->size - f->pos;
    if (btr < n) n = btr;
    if (n) memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    if (br) *br = n;
    return LV_FS_RES_OK;
}

static lv_fs_res_t te_fs_seek(lv_fs_drv_t *drv, void *file, uint32_t pos, lv_fs_whence_t whence)
{
    (void)drv;
    te_font_file_t *f = file;
    uint64_t base = whence == LV_FS_SEEK_CUR ? f->pos : whence == LV_FS_SEEK_END ? f->size : 0;
    uint64_t np = base + pos;
    f->pos = np > f->size ? f->size : (uint32_t)np;
    return LV_FS_RES_OK;
}

static lv_fs_res_t te_fs_tell(lv_fs_drv_t *drv, void *file, uint32_t *pos)
{
    (void)drv;
    *pos = ((te_font_file_t *)file)->pos;
    return LV_FS_RES_OK;
}

static void te_fs_register(te_ctx_t *c)
{
    if (c->fs_registered) return;
    lv_fs_drv_init(&c->fs_drv);
    c->fs_drv.letter = TE_FONT_LETTER;
    c->fs_drv.open_cb = te_fs_open;
    c->fs_drv.close_cb = te_fs_close;
    c->fs_drv.read_cb = te_fs_read;
    c->fs_drv.seek_cb = te_fs_seek;
    c->fs_drv.tell_cb = te_fs_tell;
    c->fs_drv.user_data = c;
    lv_fs_drv_register(&c->fs_drv);   // the context is never freed
    c->fs_registered = true;
}

lv_font_t *te_font_get(const char *name)
{
    te_ctx_t *c = g_te;
    if (!c || !name || !c->fs_registered) return NULL;
    for (uint8_t i = 0; i < c->n_fonts; i++) {
        te_font_t *f = &c->fonts[i];
        if (strcmp(f->name, name) != 0) continue;
        if (f->font || f->failed) return f->font;
        size_t n = strlen(name) + 3;
        char *path = te_malloc(n);
        if (!path) return NULL;
        snprintf(path, n, "%c:%s", TE_FONT_LETTER, name);
        f->font = lv_font_load(path);
        te_free(path);
        if (!f->font) {
            f->failed = true;
            ESP_LOGW(TE_TAG, "font '%s': lv_font_load failed, built-in font used", name);
        }
        return f->font;
    }
    return NULL;
}

// ---------------- release / unload ----------------

void te_release_locked(void)
{
    te_ctx_t *c = g_te;
    if (!c) return;
    tic_clear(&c->cache);
    for (uint8_t i = 0; i < c->n_fonts; i++) {
        if (c->fonts[i].font) lv_font_free(c->fonts[i].font);
        c->fonts[i].font = NULL;
        c->fonts[i].failed = false;
    }
    c->release_pending = false;
}

static void te_unload_locked(te_ctx_t *c)
{
    c->load_generation++;
    te_release_locked();
    te_sidecar_drop();
    te_tables_free(c);
    c->part = NULL;
    c->sd_path[0] = 0;
    c->file_size = 0;
    c->used_len = 0;
    c->custom = false;
    c->loaded = false;
    c->manifest_crc = 0;
    memset(&c->info, 0, sizeof(c->info));
    c->id[0] = '\0';
}

// ---------------- public API ----------------

esp_err_t theme_engine_init(void)
{
    te_ctx_t *c = te_ctx_ensure();
    if (!c) return ESP_ERR_NO_MEM;
    te_lock();
    te_fs_register(c);
    te_unlock();
    return theme_load(0);
}

esp_err_t theme_load(uint8_t slot)
{
    if (slot != 0) return ESP_ERR_NOT_FOUND;   // slot 0 resolves the selected SD theme
    te_ctx_t *c = te_ctx_ensure();
    if (!c) return ESP_ERR_NO_MEM;
    te_lock();
    if (c->n_live || c->n_preparing) {
        ESP_LOGE(TE_TAG, "theme_load: %u live page(s), %u preparing, current theme kept",
                 (unsigned)c->n_live, (unsigned)c->n_preparing);
        te_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    te_unload_locked(c);
    esp_err_t err = te_load_custom(c);
    if (err == ESP_OK) {
        c->custom = true;
        c->loaded = true;
        te_sidecar_reload();
        ESP_LOGI(TE_TAG, "theme '%s' v%s: %u pages, %u images, %u fonts, %u bytes used",
                 c->id, c->info.version, (unsigned)c->n_pages, (unsigned)c->n_images,
                 (unsigned)c->n_fonts, (unsigned)c->used_len);
    } else {
        te_set_default(c);
    }
    te_unlock();
    return err;
}

esp_err_t theme_get_info(theme_info_t *info)
{
    te_ctx_t *c = g_te;
    if (!c) return ESP_ERR_INVALID_STATE;
    te_lock();
    esp_err_t err = c->loaded ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && info) *info = c->info;
    te_unlock();
    return err;
}

uint8_t theme_page_list_count(void)
{
    te_ctx_t *c = g_te;
    if (!c) return 0;
    te_lock();
    uint8_t n = c->custom ? c->n_pages : 0;
    te_unlock();
    return n;
}

const char *theme_page_list_at(uint8_t index)
{
    te_ctx_t *c = g_te;
    if (!c) return NULL;
    te_lock();
    const char *id = (c->custom && index < c->n_pages) ? c->pages[index].id : NULL;
    te_unlock();
    return id;
}

uint8_t theme_page_default_index(void)
{
    te_ctx_t *c = g_te;
    if (!c) return 0;
    te_lock();
    int i = c->custom ? te_page_find("main_gauge") : -1;
    te_unlock();
    return i > 0 ? (uint8_t)i : 0;
}

void theme_release_resources(void)
{
    te_ctx_t *c = g_te;
    if (!c) return;
    te_lock();
    c->load_generation++; // also discard any unpublished boot preparation
    if (c->n_live || c->n_preparing) {
        c->release_pending = true;
        ESP_LOGW(TE_TAG, "release: %u live page(s), %u preparing, deferred",
                 (unsigned)c->n_live, (unsigned)c->n_preparing);
    } else {
        te_release_locked();
    }
    te_unlock();
}

void theme_unload(void)
{
    te_ctx_t *c = g_te;
    if (!c) return;
    te_lock();
    if (c->n_live || c->n_preparing) {
        ESP_LOGE(TE_TAG, "theme_unload: %u live page(s), %u preparing, refused",
                 (unsigned)c->n_live, (unsigned)c->n_preparing);
    } else {
        te_unload_locked(c);
    }
    te_unlock();
}
