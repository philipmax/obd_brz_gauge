/*
 * vp_manager - vehicle profile lifecycle (see vp_manager.h).
 *
 * NVS model (nvs_app "veh"):
 *   id         profile run at boot ("" = obd2.generic)
 *   state      state of id: PENDING (under verification) / ACTIVE / FAILED
 *              (a PENDING profile failed and the previous one runs instead)
 *   pending    profile to switch to at the next boot ("obd2.generic" marks a
 *              reset to generic; "" = none)
 *   prev_id    written by nvs_app_veh_id_set(); target of rollback
 *   boot_fail  boots in a row where the PENDING profile failed verification
 *   blob       source JSON of id (compiled profile when the JSON exceeds 8 KB)
 *   last_err   reason of the last automatic rollback (shown while FAILED)
 *
 * Verification of a PENDING profile: from 20 s after the ELM327 link came up,
 * it passes once K = min(3, driven signals) distinct slots answered (obd.rpm
 * among them when the profile has it) and ATRV read >= 13.0 V (engine
 * running). A boot is counted as failed only when the engine ran (>= 13.0 V)
 * for the window without enough answers; three such boots roll back.
 *
 * Crash counter (nvs_app sys/crash_cnt): +1 at boot after a panic or watchdog
 * reset, cleared after 60 s of uptime. >= 3 enters safe mode: obd2.generic for
 * this boot, no SD imports (sd_import checks vp_manager_safe_mode()).
 */
#include "vehicle/vp_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_obd_dsp/signal_registry.h"
#include "bsp_obd_dsp/config_part.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/sd_card.h"
#include "vehicle/vp_builtin.h"

static const char *TAG = "vp_manager";

#define VPM_DIR_REL         "/vehicles"
#define VPM_CFG_DIR         CONFIG_MOUNT_POINT VPM_DIR_REL
#define VPM_SD_DIR          SD_DIR_CFG VPM_DIR_REL
#define VPM_PATH_LEN        96
#define VPM_ERR_LEN         112

#define VPM_VERIFY_DELAY_US (20LL * 1000 * 1000)
#define VPM_VERIFY_MIN_V    13.0f
#define VPM_VERIFY_K        3
#define VPM_BOOT_FAIL_MAX   3
#define VPM_CRASH_MAX       3
#define VPM_CRASH_CLEAR_US  (60LL * 1000 * 1000)
#define VPM_KEY_ERR         "last_err"

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static const vp_profile_t *s_profile;        /* NULL = builtin generic */
static veh_state_t s_state = VEH_STATE_NONE;
static bool        s_inited;
static bool        s_safe;
static bool        s_crash_checked;
static uint8_t     s_crash_n;
static char        s_err[VPM_ERR_LEN];
static char        s_failed_id[VPM_ID_LEN];  /* rolled back from (state FAILED) */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static esp_timer_handle_t s_crash_timer;

/* verification (written by the ELM327 task only) */
static bool     s_verify;
static bool     s_verified;
static bool     s_fail_counted;
static bool     s_need_rpm;
static uint8_t  s_need_k;
static uint64_t s_resp_mask;
static float    s_vmax;
static int64_t  s_link_us;

/* Legacy nvs_user_cfg_t.vehicle_profile_idx order (4b056c0 compiled-in table). */
static const char *const k_legacy_ids[] = {
    VPM_GENERIC_ID,       /*  0 */
    "subaru.zc6.can",     /*  1 */
    "subaru.zc6.pid",     /*  2 */
    "subaru.zd8.obd",     /*  3 */
    "subaru.zd8.pid",     /*  4 */
    "mazda.mx5nd",        /*  5 */
    "bmw.fg",             /*  6 */
    "toyota.supra_a90",   /*  7 */
    "bmw.g.obd",          /*  8 */
    "bmw.e",              /*  9 */
    "mini.jcw_f56",       /* 10 */
    "mini.r55",           /* 11 */
    "porsche.997_2",      /* 12 */
    "porsche.997_1",      /* 13 */
    "alfa.giulia_20t",    /* 14 */
    "jeep.generic",       /* 15 */
    "honda.integra",      /* 16 */
};
#define VPM_LEGACY_N (sizeof(k_legacy_ids) / sizeof(k_legacy_ids[0]))

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void lock(void)
{
    /* First use is always from app_main (sd_import / init) before other tasks exist. */
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", s_err);
}

static void *psram_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return heap_caps_malloc(size ? size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void psram_free(void *ctx, void *ptr)
{
    (void)ctx;
    heap_caps_free(ptr);
}

static const vp_allocator_t s_alloc = { psram_alloc, psram_free, NULL };

static bool is_generic(const char *id)
{
    return !id || !id[0] || !strcmp(id, VPM_GENERIC_ID);
}

/* Profile ids become file names: [a-z0-9._-], no leading '.', no "..". */
static bool id_ok(const char *id)
{
    size_t n, i;

    if (!id) return false;
    n = strlen(id);
    if (n == 0 || n >= VPM_ID_LEN || id[0] == '.' || strstr(id, "..")) return false;
    for (i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static void cfg_path(char *out, size_t n, const char *id)
{
    snprintf(out, n, "%s/%s.json", VPM_CFG_DIR, id);
}

static void sd_path(char *out, size_t n, const char *id)
{
    snprintf(out, n, "%s/%s.json", VPM_SD_DIR, id);
}

static uint8_t legacy_idx_of(const char *id)
{
    size_t i;
    if (is_generic(id)) return 0;
    for (i = 1; i < VPM_LEGACY_N; i++) {
        if (!strcmp(k_legacy_ids[i], id)) return (uint8_t)i;
    }
    return 0;
}

static void mirror_idx(const char *id)
{
    esp_err_t e = nvs_cfg_vehicle_idx_mirror_set(legacy_idx_of(id));
    if (e != ESP_OK) ESP_LOGW(TAG, "legacy idx mirror: %s", esp_err_to_name(e));
}

/* NVS id of the profile run at boot, "obd2.generic" normalised to "". */
static void stored_id(char *out, size_t n)
{
    if (nvs_app_veh_id_get(out, n) != ESP_OK) out[0] = '\0';
    if (is_generic(out)) out[0] = '\0';
}

static void pending_id(char *out, size_t n)
{
    if (nvs_app_veh_pending_id_get(out, n) != ESP_OK) out[0] = '\0';
}

static char *dup_psram(const void *src, size_t len)
{
    char *b = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) return NULL;
    memcpy(b, src, len);
    b[len] = '\0';
    return b;
}

static const char *first_error(const vp_issues_t *is)
{
    static char buf[VPM_ERR_LEN];
    size_t i;

    for (i = 0; is && i < is->count; i++) {
        if (is->items[i].severity == VP_SEV_ERROR) {
            snprintf(buf, sizeof(buf), "%s %s: %s", is->items[i].code ? is->items[i].code : "",
                     is->items[i].path, is->items[i].msg);
            return buf;
        }
    }
    return "invalid profile";
}

/* ------------------------------------------------------------------ */
/* minimal top-level JSON lookup (list without cJSON)                  */
/* ------------------------------------------------------------------ */

static size_t js_ws(const char *s, size_t n, size_t i)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    return i;
}

static size_t js_string_end(const char *s, size_t n, size_t i)   /* i at '"' -> past closing '"' */
{
    for (i++; i < n; i++) {
        if (s[i] == '\\') i++;
        else if (s[i] == '"') return i + 1;
    }
    return n;
}

static size_t js_skip_value(const char *s, size_t n, size_t i)
{
    int depth = 0;

    if (i < n && s[i] == '"') return js_string_end(s, n, i);
    while (i < n) {
        char c = s[i];
        if (c == '"') {
            i = js_string_end(s, n, i);
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (depth == 0) return i;
            if (--depth == 0) return i + 1;
        } else if (c == ',' && depth == 0) {
            return i;
        }
        i++;
    }
    return n;
}

/* Value of top-level key (pointer to its first char), NULL if absent. */
static const char *js_top(const char *s, size_t n, const char *key, size_t *vlen)
{
    size_t i = js_ws(s, n, 0), kl = strlen(key);

    if (i >= n || s[i] != '{') return NULL;
    i++;
    while (1) {
        size_t ks, ke, vs, ve;
        i = js_ws(s, n, i);
        if (i >= n || s[i] != '"') return NULL;
        ks = i + 1;
        ke = js_string_end(s, n, i);
        i = js_ws(s, n, ke);
        if (i >= n || s[i] != ':') return NULL;
        vs = js_ws(s, n, i + 1);
        ve = js_skip_value(s, n, vs);
        if (ke - 1 - ks == kl && !memcmp(s + ks, key, kl)) {
            *vlen = ve - vs;
            return s + vs;
        }
        i = js_ws(s, n, ve);
        if (i >= n || s[i] != ',') return NULL;
        i++;
    }
}

static void js_top_str(const char *s, size_t n, const char *key, char *out, size_t cap)
{
    size_t vl = 0, i, o = 0;
    const char *v = js_top(s, n, key, &vl);

    out[0] = '\0';
    if (!v || vl < 2 || v[0] != '"') return;
    for (i = 1; i + 1 < vl && o + 1 < cap; i++) {
        char c = v[i];
        if (c == '\\' && i + 2 < vl) {
            c = v[++i];
            if (c == 'u') {
                i += 4;
                c = '?';
            }
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

static long js_top_int(const char *s, size_t n, const char *key, long def)
{
    char tmp[16];
    size_t vl = 0;
    const char *v = js_top(s, n, key, &vl);

    if (!v || vl == 0 || vl >= sizeof(tmp)) return def;
    memcpy(tmp, v, vl);
    tmp[vl] = '\0';
    return strtol(tmp, NULL, 10);
}

/* ------------------------------------------------------------------ */
/* storage                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t read_cfg_file(const char *path, char **out, size_t *out_len)
{
    struct stat st;
    size_t got = 0;
    char *buf;
    esp_err_t e;

    if (!config_part_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    if (st.st_size <= 0 || st.st_size > VP_MAX_JSON_BYTES) return ESP_ERR_INVALID_SIZE;
    buf = (char *)heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;
    e = config_part_read(path, buf, (size_t)st.st_size, &got);
    if (e != ESP_OK) {
        heap_caps_free(buf);
        return e;
    }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return ESP_OK;
}

static esp_err_t read_sd_file(const char *path, char **out, size_t *out_len)
{
    void *raw = NULL;
    size_t len = 0;
    esp_err_t e;

    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!sd_card_exists(path)) return ESP_ERR_NOT_FOUND;
    e = sd_card_read_file_alloc(path, VP_MAX_JSON_BYTES, &raw, &len);
    if (e != ESP_OK) return e;
    *out = dup_psram(raw, len);   /* NUL-terminated copy */
    heap_caps_free(raw);
    if (!*out) return ESP_ERR_NO_MEM;
    *out_len = len;
    return ESP_OK;
}

/* veh/blob into a PSRAM buffer (JSON or compiled profile). */
static esp_err_t read_blob(char **out, size_t *out_len)
{
    size_t len = 0;
    char *buf;
    esp_err_t e = nvs_app_veh_blob_get(NULL, &len);

    if (e != ESP_OK) return e;
    if (len == 0 || len > NVS_APP_VEH_BLOB_MAX) return ESP_ERR_NOT_FOUND;
    buf = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;
    e = nvs_app_veh_blob_get(buf, &len);
    if (e != ESP_OK) {
        heap_caps_free(buf);
        return e;
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return ESP_OK;
}

static bool blob_is_compiled(const char *b, size_t len)
{
    uint32_t magic;
    if (len < sizeof(vp_profile_t)) return false;
    memcpy(&magic, b, sizeof(magic));
    return magic == VP_PROFILE_MAGIC;
}

enum { SRC_NONE = 0, SRC_BUILTIN, SRC_CONFIG, SRC_SD, SRC_BLOB };

/* Source JSON of id: builtin, config partition, SD mirror, then the NVS copy
 * of the profile run at boot. Caller frees *out. */
static esp_err_t read_source(const char *id, char **out, size_t *out_len, int *from)
{
    char path[VPM_PATH_LEN], cur[NVS_APP_VEH_ID_LEN];
    esp_err_t e;

    *out = NULL;
    *out_len = 0;
    if (from) *from = SRC_NONE;
    if (is_generic(id)) {
        *out = dup_psram(vp_builtin_generic_json, vp_builtin_generic_json_len);
        if (!*out) return ESP_ERR_NO_MEM;
        *out_len = vp_builtin_generic_json_len;
        if (from) *from = SRC_BUILTIN;
        return ESP_OK;
    }
    if (!id_ok(id)) return ESP_ERR_INVALID_ARG;
    cfg_path(path, sizeof(path), id);
    e = read_cfg_file(path, out, out_len);
    if (e == ESP_OK) {
        if (from) *from = SRC_CONFIG;
        return ESP_OK;
    }
    sd_path(path, sizeof(path), id);
    if (read_sd_file(path, out, out_len) == ESP_OK) {
        if (from) *from = SRC_SD;
        return ESP_OK;
    }
    stored_id(cur, sizeof(cur));
    if (!strcmp(cur, id) && read_blob(out, out_len) == ESP_OK) {
        if (!blob_is_compiled(*out, *out_len)) {
            if (from) *from = SRC_BLOB;
            return ESP_OK;
        }
        heap_caps_free(*out);
        *out = NULL;
        *out_len = 0;
    }
    return ESP_ERR_NOT_FOUND;
}

static bool profile_exists(const char *id)
{
    char path[VPM_PATH_LEN];

    if (is_generic(id)) return true;
    cfg_path(path, sizeof(path), id);
    if (config_part_is_mounted() && config_part_exists(path)) return true;
    sd_path(path, sizeof(path), id);
    return sd_card_is_mounted() && sd_card_exists(path);
}

/* Store source JSON as /config/vehicles/<id>.json (config_part mirrors it to
 * SD); SD mirror only when the config partition is unavailable. */
static esp_err_t write_source(const char *id, const char *json, size_t len)
{
    char path[VPM_PATH_LEN];

    if (config_part_is_mounted()) {
        cfg_path(path, sizeof(path), id);
        return config_part_write(path, json, len);
    }
    if (sd_card_is_mounted()) {
        sd_card_mkdir(SD_DIR_CFG);
        sd_card_mkdir(VPM_SD_DIR);
        sd_path(path, sizeof(path), id);
        return sd_card_write_file_atomic(path, json, len);
    }
    return ESP_ERR_INVALID_STATE;
}

static bool same_content(const char *id, const char *json, size_t len)
{
    char path[VPM_PATH_LEN];
    char *old = NULL;
    size_t old_len = 0;
    bool same;

    cfg_path(path, sizeof(path), id);
    if (read_cfg_file(path, &old, &old_len) != ESP_OK) {
        sd_path(path, sizeof(path), id);
        if (config_part_is_mounted() || read_sd_file(path, &old, &old_len) != ESP_OK) return false;
    }
    same = old_len == len && !memcmp(old, json, len);
    heap_caps_free(old);
    return same;
}

/* Compile id from storage into PSRAM. NULL + why on failure. */
static vp_profile_t *load_profile(const char *id, char *why, size_t why_n)
{
    vp_issue_t one;
    vp_issues_t is;
    vp_profile_t *p = NULL;
    char *src = NULL;
    size_t len = 0;
    int from = SRC_NONE;
    vp_compile_err_t r;
    esp_err_t e = read_source(id, &src, &len, &from);

    if (e == ESP_OK) {
        vp_issues_init(&is, &one, 1);
        r = vp_compile_json(src, len, vp_validate_default_cfg(), &s_alloc, &p, &is);
        if (r != VP_COMPILE_OK) {
            snprintf(why, why_n, "%s", r == VP_COMPILE_ERR_NOMEM ? "out of memory" : first_error(&is));
            heap_caps_free(src);
            return NULL;
        }
        if (strcmp(p->id, id) != 0) {
            snprintf(why, why_n, "file holds id %s", p->id);
            vp_profile_free(p, &s_alloc);
            heap_caps_free(src);
            return NULL;
        }
        /* Restore a config partition copy lost to a wipe. */
        if ((from == SRC_SD || from == SRC_BLOB) && config_part_is_mounted()) {
            char path[VPM_PATH_LEN];
            cfg_path(path, sizeof(path), id);
            if (config_part_write(path, src, len) == ESP_OK) ESP_LOGI(TAG, "%s restored to %s", id, path);
        }
        heap_caps_free(src);
        return p;
    }
    /* Last resort: compiled copy in veh/blob (profile JSON larger than the blob). */
    {
        char cur[NVS_APP_VEH_ID_LEN];
        stored_id(cur, sizeof(cur));
        if (!strcmp(cur, id) && read_blob(&src, &len) == ESP_OK) {
            if (blob_is_compiled(src, len) && vp_profile_check(src, len) &&
                !strcmp(((const vp_profile_t *)(const void *)src)->id, id)) {
                return (vp_profile_t *)(void *)src;   /* PSRAM, 4-byte aligned */
            }
            heap_caps_free(src);
        }
    }
    snprintf(why, why_n, "%s", e == ESP_ERR_NOT_FOUND ? "not installed" : esp_err_to_name(e));
    return NULL;
}

/* Keep veh/blob equal to the profile run at boot (written only when it differs). */
static void refresh_blob(const vp_profile_t *p)
{
    char *src = NULL, *old = NULL;
    size_t len = 0, old_len = 0;
    const void *want;
    size_t want_len;
    int from = SRC_NONE;

    if (read_source(p->id, &src, &len, &from) != ESP_OK || from == SRC_BLOB) {
        heap_caps_free(src);
        return;
    }
    if (len <= NVS_APP_VEH_BLOB_MAX) {
        want = src;
        want_len = len;
    } else if (p->total_size <= NVS_APP_VEH_BLOB_MAX) {
        want = p;
        want_len = p->total_size;
    } else {
        ESP_LOGW(TAG, "%s too large for the NVS copy", p->id);
        heap_caps_free(src);
        return;
    }
    if (read_blob(&old, &old_len) != ESP_OK || old_len != want_len || memcmp(old, want, want_len)) {
        esp_err_t e = nvs_app_veh_blob_set(want, want_len);
        if (e != ESP_OK) ESP_LOGW(TAG, "veh blob write: %s", esp_err_to_name(e));
    }
    heap_caps_free(old);
    heap_caps_free(src);
}

/* ------------------------------------------------------------------ */
/* crash counter / safe mode                                           */
/* ------------------------------------------------------------------ */

static void crash_clear_cb(void *arg)
{
    (void)arg;
    if (nvs_app_crash_count_set(0) == ESP_OK) ESP_LOGI(TAG, "60 s uptime: crash counter cleared");
}

static void crash_check(void)
{
    esp_reset_reason_t why;
    uint8_t n;

    if (s_crash_checked) return;
    s_crash_checked = true;
    why = esp_reset_reason();
    n = nvs_app_crash_count_get();
    if (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT) {
        if (n < 255) n++;
        nvs_app_crash_count_set(n);
    }
    s_crash_n = n;
    s_safe = n >= VPM_CRASH_MAX;
    if (n > 0) {
        const esp_timer_create_args_t args = {
            .callback = crash_clear_cb,
            .name = "vpm_crash",
        };
        if (esp_timer_create(&args, &s_crash_timer) == ESP_OK) {
            esp_timer_start_once(s_crash_timer, VPM_CRASH_CLEAR_US);
        }
    }
    if (s_safe) ESP_LOGE(TAG, "%u abnormal resets in a row: SAFE MODE", (unsigned)n);
}

/* ------------------------------------------------------------------ */
/* boot-time transitions                                               */
/* ------------------------------------------------------------------ */

static void count_boot_fail(void)
{
    uint8_t n = nvs_app_veh_boot_fail_get();
    if (n < 255) n++;
    nvs_app_veh_boot_fail_set(n);
    ESP_LOGW(TAG, "pending profile: failed boot %u/%u", (unsigned)n, VPM_BOOT_FAIL_MAX);
}

/* pending -> current. *pp receives the compiled profile when id is promoted. */
static void promote(const char *pend, char *cur, size_t cur_n, veh_state_t *st, vp_profile_t **pp)
{
    char why[VPM_ERR_LEN / 2];
    vp_profile_t *p;

    if (is_generic(pend)) {
        nvs_app_veh_id_set("");
        nvs_app_veh_state_set(VEH_STATE_NONE);
        nvs_app_veh_boot_fail_set(0);
        nvs_app_veh_pending_id_set("");
        nvs_app_veh_blob_erase();
        nvs_app_erase_key("veh", VPM_KEY_ERR);
        cur[0] = '\0';
        *st = VEH_STATE_NONE;
        ESP_LOGI(TAG, "switched to %s", VPM_GENERIC_ID);
        return;
    }
    p = load_profile(pend, why, sizeof(why));
    nvs_app_veh_pending_id_set("");
    if (!p) {
        set_err("cannot activate %s: %s", pend, why);
        return;
    }
    nvs_app_veh_id_set(pend);
    nvs_app_veh_state_set(VEH_STATE_PENDING);
    nvs_app_veh_boot_fail_set(0);
    nvs_app_erase_key("veh", VPM_KEY_ERR);
    snprintf(cur, cur_n, "%s", pend);
    *st = VEH_STATE_PENDING;
    *pp = p;
    ESP_LOGI(TAG, "activated %s rev %u (verifying)", p->id, (unsigned)p->rev);
}

static void auto_rollback(char *cur, size_t cur_n, veh_state_t *st, vp_profile_t **pp)
{
    char failed[VPM_ID_LEN], target[NVS_APP_VEH_ID_LEN], why[VPM_ERR_LEN / 2];
    vp_profile_t *p = NULL;

    snprintf(failed, sizeof(failed), "%s", cur);
    if (nvs_app_veh_prev_id_get(target, sizeof(target)) != ESP_OK || is_generic(target) ||
        !strcmp(target, failed)) {
        target[0] = '\0';
    }
    if (target[0]) {
        p = load_profile(target, why, sizeof(why));
        if (!p) {
            ESP_LOGW(TAG, "rollback target %s unusable (%s)", target, why);
            target[0] = '\0';
        }
    }
    nvs_app_veh_id_set(target);
    nvs_app_veh_state_set(VEH_STATE_FAILED);
    nvs_app_veh_boot_fail_set(0);
    mirror_idx(target);
    set_err("%s failed verification on %u boots; rolled back to %s", failed, VPM_BOOT_FAIL_MAX,
            target[0] ? target : VPM_GENERIC_ID);
    nvs_app_set_str("veh", VPM_KEY_ERR, s_err);
    snprintf(cur, cur_n, "%s", target);
    *st = VEH_STATE_FAILED;
    *pp = p;
}

static void verify_setup(const vp_profile_t *p)
{
    const vp_signal_t *sig = vp_profile_signals(p);
    int i, n = 0;

    s_need_rpm = false;
    for (i = 0; i < p->n_sig; i++) {
        if (sig[i].src_count == 0 || sig[i].derive != VP_DERIVE_NONE) continue;
        n++;
        if (!strcmp(sig[i].key, "obd.rpm")) s_need_rpm = true;
    }
    s_need_k = (uint8_t)(n < VPM_VERIFY_K ? n : VPM_VERIFY_K);
    s_resp_mask = 0;
    s_vmax = 0.0f;
    s_link_us = 0;
    s_verified = false;
    s_fail_counted = false;
    s_verify = true;
}

void vp_manager_init(void)
{
    char cur[NVS_APP_VEH_ID_LEN], pend[NVS_APP_VEH_ID_LEN], why[VPM_ERR_LEN / 2];
    veh_state_t st;
    vp_profile_t *p = NULL;

    lock();
    if (s_inited) {
        unlock();
        return;
    }
    crash_check();
    stored_id(cur, sizeof(cur));
    st = nvs_app_veh_state_get();

    if (s_safe) {
        if (st == VEH_STATE_PENDING) count_boot_fail();
        set_err("safe mode: %u crashes in a row, running %s", (unsigned)s_crash_n, VPM_GENERIC_ID);
        s_profile = NULL;
        s_state = VEH_STATE_NONE;
        s_inited = true;
        unlock();
        return;
    }

    pending_id(pend, sizeof(pend));
    if (pend[0]) promote(pend, cur, sizeof(cur), &st, &p);
    if (st == VEH_STATE_PENDING && cur[0] && nvs_app_veh_boot_fail_get() >= VPM_BOOT_FAIL_MAX) {
        if (p) vp_profile_free(p, &s_alloc);
        p = NULL;
        auto_rollback(cur, sizeof(cur), &st, &p);
    }

    if (cur[0] && !p) {
        p = load_profile(cur, why, sizeof(why));
        if (!p) {
            set_err("%s: %s; running %s", cur, why, VPM_GENERIC_ID);
            snprintf(s_failed_id, sizeof(s_failed_id), "%s", cur);
            st = VEH_STATE_FAILED;   /* this boot only */
        }
    }
    if (p) {
        if (st == VEH_STATE_NONE || st == VEH_STATE_INSTALLED) {
            st = VEH_STATE_ACTIVE;   /* e.g. set by the migration without a state */
            nvs_app_veh_state_set(st);
        }
        refresh_blob(p);
    } else if (st != VEH_STATE_FAILED) {
        st = VEH_STATE_NONE;
    }
    if (st == VEH_STATE_FAILED) {
        if (!s_err[0]) nvs_app_get_str("veh", VPM_KEY_ERR, s_err, sizeof(s_err));
        if (!s_failed_id[0] && nvs_app_veh_prev_id_get(s_failed_id, sizeof(s_failed_id)) != ESP_OK) {
            s_failed_id[0] = '\0';
        }
    }
    s_profile = p;
    s_state = st;
    if (p && st == VEH_STATE_PENDING) verify_setup(p);
    s_inited = true;
    unlock();
    ESP_LOGI(TAG, "vehicle %s rev %u state %d%s", vp_manager_active_id(), (unsigned)vp_manager_active()->rev,
             (int)s_state, s_verify ? " (verification armed)" : "");
}

/* ------------------------------------------------------------------ */
/* queries                                                             */
/* ------------------------------------------------------------------ */

const vp_profile_t *vp_manager_active(void)
{
    return s_profile ? s_profile : vp_builtin_generic();
}

const char *vp_manager_active_id(void)
{
    return vp_manager_active()->id;
}

veh_state_t vp_manager_active_state(void)
{
    return s_state;
}

bool vp_manager_safe_mode(void)
{
    if (!s_crash_checked) {
        lock();
        crash_check();
        unlock();
    }
    return s_safe;
}

const char *vp_manager_last_error(void)
{
    return s_err;
}

bool vp_manager_has_signal(int slot)
{
    const vp_profile_t *p = vp_manager_active();
    const char *key;

    if (slot < 0 || slot >= SIG_SLOT_COUNT) return false;
    if (slot < SIG_STD_COUNT) {
        if (slot >= SIG_OBD_FIRST + SIG_OBD_COUNT) return false;
        key = sig_registry[slot].key;
    } else {
        char key_buf[32];
        if (!signal_store_copy_key(slot, key_buf, sizeof(key_buf))) return false;
        return vp_profile_find_signal(p, key_buf) >= 0;
    }
    return key && vp_profile_find_signal(p, key) >= 0;
}

uint8_t vp_manager_gear_count(void)
{
    const vp_profile_t *p = vp_manager_active();
    if (p->final_drive <= 0.0f || p->tire_m <= 0.0f) return 0;
    return p->gear_count;
}

const char *vp_manager_legacy_id(uint8_t idx)
{
    if (idx == 0 || idx >= VPM_LEGACY_N) return NULL;
    return k_legacy_ids[idx];
}

/* ------------------------------------------------------------------ */
/* install / activate / remove                                         */
/* ------------------------------------------------------------------ */

esp_err_t vp_manager_install_json(const char *json, size_t len, vp_issues_t *issues,
                                  char *out_id, size_t out_id_len)
{
    vp_issue_t one;
    vp_issues_t local;
    vp_profile_t *p = NULL;
    vp_compile_err_t r;
    char id[VPM_ID_LEN], cur[NVS_APP_VEH_ID_LEN], pend[NVS_APP_VEH_ID_LEN];
    esp_err_t e;

    if (out_id && out_id_len) out_id[0] = '\0';
    if (!json || len == 0) return ESP_ERR_INVALID_ARG;
    if (!issues) {
        vp_issues_init(&local, &one, 1);
        issues = &local;
    }
    lock();
    r = vp_compile_json(json, len, vp_validate_default_cfg(), &s_alloc, &p, issues);
    if (r != VP_COMPILE_OK) {
        e = r == VP_COMPILE_ERR_NOMEM ? ESP_ERR_NO_MEM
          : (r == VP_COMPILE_ERR_PARSE || r == VP_COMPILE_ERR_INVALID) ? ESP_ERR_INVALID_ARG : ESP_FAIL;
        set_err("install: %s", e == ESP_ERR_NO_MEM ? "out of memory" : first_error(issues));
        unlock();
        return e;
    }
    snprintf(id, sizeof(id), "%s", p->id);
    vp_profile_free(p, &s_alloc);
    if (out_id && out_id_len) snprintf(out_id, out_id_len, "%s", id);
    if (is_generic(id)) {
        set_err("install: %s is built in; use another id", VPM_GENERIC_ID);
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (!id_ok(id)) {
        set_err("install: id '%s' is not a valid file name", id);
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (same_content(id, json, len)) {
        ESP_LOGI(TAG, "%s already installed (unchanged)", id);
        s_err[0] = '\0';
        unlock();
        return ESP_OK;
    }
    e = write_source(id, json, len);
    if (e != ESP_OK) {
        set_err("install %s: storage %s", id, esp_err_to_name(e));
        unlock();
        return e;
    }
    /* New content for the profile run at boot: verify it again from the next boot. */
    stored_id(cur, sizeof(cur));
    pending_id(pend, sizeof(pend));
    if (!strcmp(cur, id) && !pend[0]) nvs_app_veh_pending_id_set(id);
    s_err[0] = '\0';
    ESP_LOGI(TAG, "installed %s (%u bytes)", id, (unsigned)len);
    unlock();
    return ESP_OK;
}

esp_err_t vp_manager_reset_to_generic(void)
{
    char cur[NVS_APP_VEH_ID_LEN];
    esp_err_t e;

    lock();
    stored_id(cur, sizeof(cur));
    /* Already generic and healthy: just cancel a pending switch. */
    if (!cur[0] && nvs_app_veh_state_get() != VEH_STATE_FAILED) e = nvs_app_veh_pending_id_set("");
    else e = nvs_app_veh_pending_id_set(VPM_GENERIC_ID);
    if (e == ESP_OK) {
        mirror_idx(VPM_GENERIC_ID);
        s_err[0] = '\0';
    } else {
        set_err("reset: nvs %s", esp_err_to_name(e));
    }
    unlock();
    return e;
}

esp_err_t vp_manager_activate(const char *id)
{
    char cur[NVS_APP_VEH_ID_LEN];
    esp_err_t e;

    if (!id || !id[0]) return ESP_ERR_INVALID_ARG;
    if (is_generic(id)) return vp_manager_reset_to_generic();
    lock();
    if (!id_ok(id)) {
        set_err("activate: invalid id '%s'", id);
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (!profile_exists(id)) {
        set_err("activate: %s is not installed", id);
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    stored_id(cur, sizeof(cur));
    if (!strcmp(cur, id) && nvs_app_veh_state_get() != VEH_STATE_FAILED) {
        e = nvs_app_veh_pending_id_set("");   /* already the boot profile: cancel a pending switch */
    } else {
        e = nvs_app_veh_pending_id_set(id);
    }
    if (e == ESP_OK) {
        mirror_idx(id);
        s_err[0] = '\0';
        ESP_LOGI(TAG, "%s pending for next boot", id);
    } else {
        set_err("activate %s: nvs %s", id, esp_err_to_name(e));
    }
    unlock();
    return e;
}

esp_err_t vp_manager_rollback(void)
{
    char prev[NVS_APP_VEH_ID_LEN], cur[NVS_APP_VEH_ID_LEN];

    if (nvs_app_veh_prev_id_get(prev, sizeof(prev)) != ESP_OK) prev[0] = '\0';
    stored_id(cur, sizeof(cur));
    if (is_generic(prev) || !strcmp(prev, cur)) return vp_manager_reset_to_generic();
    return vp_manager_activate(prev);
}

esp_err_t vp_manager_remove(const char *id)
{
    char path[VPM_PATH_LEN], cur[NVS_APP_VEH_ID_LEN], pend[NVS_APP_VEH_ID_LEN];
    esp_err_t e;

    if (!id || !id[0]) return ESP_ERR_INVALID_ARG;
    lock();
    if (is_generic(id)) {
        set_err("remove: %s is built in", VPM_GENERIC_ID);
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (!id_ok(id)) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    stored_id(cur, sizeof(cur));
    if (!strcmp(id, vp_manager_active_id()) || !strcmp(id, cur)) {
        set_err("remove: %s is the active profile", id);
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!profile_exists(id)) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    pending_id(pend, sizeof(pend));
    if (!strcmp(pend, id)) nvs_app_veh_pending_id_set("");
    if (config_part_is_mounted()) {
        cfg_path(path, sizeof(path), id);
        e = config_part_remove(path);   /* removes the SD mirror too */
    } else {
        sd_path(path, sizeof(path), id);
        e = sd_card_remove(path);
    }
    if (e != ESP_OK) set_err("remove %s: %s", id, esp_err_to_name(e));
    else ESP_LOGI(TAG, "removed %s", id);
    unlock();
    return e;
}

/* ------------------------------------------------------------------ */
/* list / read / export                                                */
/* ------------------------------------------------------------------ */

static veh_state_t entry_state(const char *id, bool active, bool pending)
{
    if (active) return s_state == VEH_STATE_FAILED ? VEH_STATE_ACTIVE : (s_state ? s_state : VEH_STATE_ACTIVE);
    if (pending) return VEH_STATE_PENDING;
    if (s_state == VEH_STATE_FAILED && s_failed_id[0] && !strcmp(id, s_failed_id)) return VEH_STATE_FAILED;
    return VEH_STATE_INSTALLED;
}

/* Reopen after each removal: FAT/LittleFS directory cursors need not remain
 * stable when entries are deleted. Only installed JSON files are in scope. */
static esp_err_t clear_vehicle_dir(const char *dir, bool on_cfg)
{
    for (;;) {
        char path[VPM_PATH_LEN] = {0};
        DIR *d = opendir(dir);
        if (!d) return errno == ENOENT ? ESP_OK : ESP_FAIL;
        esp_err_t err = ESP_OK;
        for (;;) {
            errno = 0;
            struct dirent *de = readdir(d);
            if (!de) {
                if (errno) err = ESP_FAIL;
                break;
            }
            size_t n = strlen(de->d_name);
            if (n <= 5 || strcmp(de->d_name + n - 5, ".json")) continue;
            int written = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            if (written < 0 || (size_t)written >= sizeof(path)) err = ESP_ERR_INVALID_SIZE;
            break;
        }
        closedir(d);
        if (err != ESP_OK) return err;
        if (!path[0]) return ESP_OK;
        err = on_cfg ? config_part_remove(path) : sd_card_remove(path);
        if (err != ESP_OK) return err;
    }
}

esp_err_t vp_manager_clear_installed(void)
{
    /* Do not report a cleared mirror when the card could not be accessed. */
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    lock();
    /* Persist a safe next boot before removing even the current profile's
     * source. Its compiled pointer remains valid until reboot. */
    esp_err_t err = nvs_app_veh_pending_id_set(VPM_GENERIC_ID);
    if (err == ESP_OK && config_part_is_mounted()) err = clear_vehicle_dir(VPM_CFG_DIR, true);
    /* config_part_remove mirrors best-effort: explicitly clear orphaned SD
     * copies too, and propagate SD failures instead of claiming success. */
    if (err == ESP_OK) err = clear_vehicle_dir(VPM_SD_DIR, false);
    if (err == ESP_OK) {
        mirror_idx(VPM_GENERIC_ID);
        s_err[0] = '\0';
    } else {
        set_err("clear installed profiles: %s", esp_err_to_name(err));
    }
    unlock();
    return err;
}

int vp_manager_list(vpm_entry_t *out, int max)
{
    const vp_profile_t *g = vp_builtin_generic();
    char pend[NVS_APP_VEH_ID_LEN];
    const char *dir_path;
    bool on_cfg = config_part_is_mounted();
    DIR *d;
    struct dirent *de;
    int n = 0;

    if (!out || max <= 0) return 0;
    lock();
    pending_id(pend, sizeof(pend));
    memset(&out[0], 0, sizeof(out[0]));
    snprintf(out[0].id, sizeof(out[0].id), "%s", VPM_GENERIC_ID);
    snprintf(out[0].name, sizeof(out[0].name), "%s", g->name);
    out[0].rev = g->rev;
    out[0].builtin = true;
    out[0].active = s_profile == NULL;
    out[0].pending = pend[0] && is_generic(pend);
    out[0].state = entry_state(VPM_GENERIC_ID, out[0].active, out[0].pending);
    n = 1;

    dir_path = on_cfg ? VPM_CFG_DIR : (sd_card_is_mounted() ? VPM_SD_DIR : NULL);
    d = dir_path ? opendir(dir_path) : NULL;
    while (d && n < max && (de = readdir(d)) != NULL) {
        char id[VPM_ID_LEN], path[VPM_PATH_LEN];
        char *src = NULL;
        size_t nl = strlen(de->d_name), len = 0;
        vpm_entry_t *e;
        esp_err_t r;

        if (nl <= 5 || nl - 5 >= sizeof(id) || strcmp(de->d_name + nl - 5, ".json")) continue;
        memcpy(id, de->d_name, nl - 5);
        id[nl - 5] = '\0';
        if (!id_ok(id) || is_generic(id)) continue;
        e = &out[n];
        memset(e, 0, sizeof(*e));
        snprintf(e->id, sizeof(e->id), "%s", id);
        snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name);
        r = on_cfg ? read_cfg_file(path, &src, &len) : read_sd_file(path, &src, &len);
        if (r == ESP_OK) {
            js_top_str(src, len, "name", e->name, sizeof(e->name));
            e->rev = (uint16_t)js_top_int(src, len, "rev", 0);
            heap_caps_free(src);
        }
        if (!e->name[0]) snprintf(e->name, sizeof(e->name), "%s", id);
        e->active = s_profile && !strcmp(s_profile->id, id);
        e->pending = !strcmp(pend, id);
        e->state = entry_state(id, e->active, e->pending);
        n++;
    }
    if (d) closedir(d);
    unlock();
    return n;
}

esp_err_t vp_manager_read_json(const char *id, char **out, size_t *out_len)
{
    esp_err_t e;

    if (!id || !out || !out_len) return ESP_ERR_INVALID_ARG;
    lock();
    e = read_source(id, out, out_len, NULL);
    unlock();
    return e;
}

esp_err_t vp_manager_export(const char *id)
{
    char path[VPM_PATH_LEN];
    char *src = NULL;
    size_t len = 0;
    esp_err_t e;

    if (!id || !id[0]) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    lock();
    e = read_source(id, &src, &len, NULL);
    if (e == ESP_OK) {
        sd_card_mkdir(SD_DIR_EXPORT);
        snprintf(path, sizeof(path), "%s/%s.vehicle.json", SD_DIR_EXPORT, is_generic(id) ? VPM_GENERIC_ID : id);
        e = sd_card_write_file_atomic(path, src, len);
        heap_caps_free(src);
    }
    if (e != ESP_OK) set_err("export %s: %s", id, esp_err_to_name(e));
    unlock();
    return e;
}

/* ------------------------------------------------------------------ */
/* verification hooks (ELM327 task)                                    */
/* ------------------------------------------------------------------ */

static void verify_eval(void)
{
    int64_t now = esp_timer_get_time();
    int got;
    bool pass;

    if (!s_verify || s_verified || s_link_us == 0 || now - s_link_us < VPM_VERIFY_DELAY_US) return;
    got = __builtin_popcountll(s_resp_mask);
    pass = got >= s_need_k && (!s_need_rpm || (s_resp_mask & (1ULL << SIG_OBD_RPM))) &&
           s_vmax >= VPM_VERIFY_MIN_V;
    if (pass) {
        s_verified = true;
        nvs_app_veh_state_set(VEH_STATE_ACTIVE);
        nvs_app_veh_boot_fail_set(0);
        s_state = VEH_STATE_ACTIVE;
        s_err[0] = '\0';
        ESP_LOGI(TAG, "%s verified (%d slots, %.1f V): ACTIVE", vp_manager_active_id(), got, (double)s_vmax);
        return;
    }
    if (!s_fail_counted && s_vmax >= VPM_VERIFY_MIN_V) {
        /* Engine ran for the whole window but the profile did not deliver. */
        s_fail_counted = true;
        count_boot_fail();
        set_err("%s: %d/%u signals answered%s", vp_manager_active_id(), got, (unsigned)s_need_k,
                s_need_rpm && !(s_resp_mask & (1ULL << SIG_OBD_RPM)) ? ", no rpm" : "");
    }
}

void vp_manager_on_elm_link(bool connected)
{
    if (!s_verify) return;
    s_link_us = connected ? esp_timer_get_time() : 0;
    if (s_link_us == 0 && connected) s_link_us = 1;
}

void vp_manager_on_voltage(float volts)
{
    if (!s_verify || s_verified) return;
    if (volts > s_vmax) s_vmax = volts;
    verify_eval();
}

void vp_manager_on_response(int slot)
{
    if (!s_verify || s_verified) return;
    if (slot >= 0 && slot < 64) s_resp_mask |= 1ULL << slot;
    verify_eval();
}
