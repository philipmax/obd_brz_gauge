// sd_import.c — /OBD/IMPORT drop folder: vehicle profiles, theme sidecars,
// theme.bin files and the ACTIVATE.TXT / RESET.TXT command files.
// Contract: see sd_import.h.
#include "app_obd_dsp/sd_import.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/first_boot_migration.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/theme_library.h"
#include "theme_engine/theme_interface.h"
#include "vehicle/vp_manager.h"
#include "vehicle/vp_validate.h"

static const char *TAG = "sd_import";

#define BATCH        24          /* names collected per directory pass */
#define MAX_PASSES   16
#define NAME_LEN     256
#define PATH_LEN     (sizeof(SD_DIR_IMPORT) + NAME_LEN + 16)
#define MAX_ISSUES   16
#define REPORT_LEN   512
#define ERRTXT_CAP   3072
#define LOCK_WAIT    pdMS_TO_TICKS(30000)
#define MAX_DIRS     32          /* theme dirs checked for stale .part files */

typedef enum { K_NONE = 0, K_VEHICLE, K_SIDECAR, K_THEME, K_ACTIVATE, K_RESET } kind_t;

/* Per-scan scratch, in PSRAM (keeps the caller's stack small). */
typedef struct {
    char        names[BATCH][NAME_LEN];
    char        src[PATH_LEN];
    char        dst[PATH_LEN];
    char        line[NAME_LEN + REPORT_LEN];
    char        report[REPORT_LEN];
    char        errtxt[ERRTXT_CAP];
    vp_issue_t  issue_buf[MAX_ISSUES];
    vp_issues_t issues;
    int         ok;
    int         err;
} scan_t;

/* ------------------------------------------------------------------ */

static kind_t kind_of(const char *name)
{
    if (name[0] == '.') return K_NONE;
    if (strcasecmp(name, "ACTIVATE.TXT") == 0) return K_ACTIVATE;
    if (strcasecmp(name, "RESET.TXT") == 0) return K_RESET;
    if (storage_has_suffix_ci(name, ".vehicle.json")) return K_VEHICLE;
    if (storage_has_suffix_ci(name, ".gaugeproj.json")) return K_SIDECAR;
    if (storage_has_suffix_ci(name, ".bin")) return K_THEME;
    return K_NONE;
}

static bool still_allowed(void)
{
    return sd_card_is_mounted() && !ota_wifi_server_is_active();
}

static void timestamp(char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2025) {
        strftime(out, len, "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        snprintf(out, len, "boot+%llus", (unsigned long long)(esp_timer_get_time() / 1000000));
    }
}

/* One CRLF line in IMPORT.LOG: "<time> OK|ERR <file>: <detail>". */
static void log_result(scan_t *s, bool ok, const char *name, const char *detail)
{
    char ts[24];
    timestamp(ts, sizeof(ts));
    snprintf(s->line, sizeof(s->line), "%s %s %s: %s", ts, ok ? "OK " : "ERR", name, detail);
    for (char *p = s->line; *p; p++) {
        if (*p == '\r' || *p == '\n') *p = ' ';
    }
    sd_card_append_line(SD_IMPORT_LOG, s->line);
    if (ok) {
        s->ok++;
        ESP_LOGI(TAG, "%s: %s", name, detail);
    } else {
        s->err++;
        ESP_LOGW(TAG, "%s: %s", name, detail);
    }
}

/* Rename IMPORT/<name> to IMPORT/<name><suffix>. */
static void mark(scan_t *s, const char *name, const char *suffix)
{
    snprintf(s->src, sizeof(s->src), SD_DIR_IMPORT "/%s", name);
    snprintf(s->dst, sizeof(s->dst), SD_DIR_IMPORT "/%s%s", name, suffix);
    if (sd_card_rename(s->src, s->dst) != ESP_OK) {
        ESP_LOGW(TAG, "rename %s -> %s failed", s->src, s->dst);
        kind_t k = kind_of(name);
        if (k == K_ACTIVATE || k == K_RESET) unlink(s->src);   /* never run a command twice */
    }
}

/* <name>.err.txt: reason line, then one line per validator issue. */
static void write_err_txt(scan_t *s, const char *name, const char *reason, bool with_issues)
{
    size_t n = (size_t)snprintf(s->errtxt, sizeof(s->errtxt), "%s\r\n", reason);
    if (n >= sizeof(s->errtxt)) n = sizeof(s->errtxt) - 1;
    if (with_issues) {
        for (size_t i = 0; i < s->issues.count && n < sizeof(s->errtxt) - 1; i++) {
            const vp_issue_t *is = &s->issues.items[i];
            int w = snprintf(s->errtxt + n, sizeof(s->errtxt) - n, "%s %s %s: %s\r\n",
                             is->code ? is->code : "E_?",
                             is->severity == VP_SEV_WARN ? "warning" : "error", is->path, is->msg);
            if (w < 0) break;
            n += (size_t)w;
            if (n >= sizeof(s->errtxt)) n = sizeof(s->errtxt) - 1;
        }
        size_t more = s->issues.n_errors + s->issues.n_warnings;
        if (more > s->issues.count && n < sizeof(s->errtxt) - 1) {
            int w = snprintf(s->errtxt + n, sizeof(s->errtxt) - n, "(%u more)\r\n",
                             (unsigned)(more - s->issues.count));
            if (w > 0) n += (size_t)w;
            if (n >= sizeof(s->errtxt)) n = sizeof(s->errtxt) - 1;
        }
    }
    snprintf(s->dst, sizeof(s->dst), SD_DIR_IMPORT "/%s.err.txt", name);
    sd_card_write_file(s->dst, s->errtxt, n);
}

static void fail(scan_t *s, const char *name, const char *reason, bool with_issues)
{
    write_err_txt(s, name, reason, with_issues);
    mark(s, name, ".err");
    log_result(s, false, name, reason);
}

static void succeed(scan_t *s, const char *name, const char *detail)
{
    snprintf(s->dst, sizeof(s->dst), SD_DIR_IMPORT "/%s.err.txt", name);
    unlink(s->dst);   /* stale result of an earlier failed attempt */
    mark(s, name, ".ok");
    log_result(s, true, name, detail);
}

/* ------------------------------------------------------------------ */

static void do_vehicle(scan_t *s, const char *name)
{
    snprintf(s->src, sizeof(s->src), SD_DIR_IMPORT "/%s", name);
    void *json = NULL;
    size_t len = 0;
    esp_err_t err = sd_card_read_file_alloc(s->src, VP_MAX_JSON_BYTES, &json, &len);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_SIZE) {
            snprintf(s->report, sizeof(s->report), "file larger than %u bytes", (unsigned)VP_MAX_JSON_BYTES);
        } else {
            snprintf(s->report, sizeof(s->report), "read failed (%s)", esp_err_to_name(err));
        }
        fail(s, name, s->report, false);
        return;
    }
    vp_issues_init(&s->issues, s->issue_buf, MAX_ISSUES);
    char id[VPM_ID_LEN] = "";
    err = vp_manager_install_json((const char *)json, len, &s->issues, id, sizeof(id));
    heap_caps_free(json);
    if (err != ESP_OK) {
        const char *why = vp_manager_last_error();
        snprintf(s->report, sizeof(s->report), "install failed: %s (%u errors)",
                 (why && why[0]) ? why : esp_err_to_name(err), (unsigned)s->issues.n_errors);
        fail(s, name, s->report, true);
        return;
    }
    snprintf(s->report, sizeof(s->report), "installed %s (%u warnings; activate via ACTIVATE.TXT or the vehicle page)",
             id, (unsigned)s->issues.n_warnings);
    succeed(s, name, s->report);
}

static void do_sidecar(scan_t *s, const char *name)
{
    snprintf(s->src, sizeof(s->src), SD_DIR_IMPORT "/%s", name);
    s->report[0] = '\0';
    esp_err_t err = theme_sidecar_install_file(s->src, s->report, sizeof(s->report));
    if (err != ESP_OK) {
        char why[96];
        snprintf(why, sizeof(why), "sidecar install failed (%s)", esp_err_to_name(err));
        if (s->report[0]) {
            size_t l = strlen(why);
            snprintf(why + l, sizeof(why) - l, ": %.60s", s->report);
        }
        fail(s, name, why, false);
        return;
    }
    if (!s->report[0]) strlcpy(s->report, "sidecar installed", sizeof(s->report));
    succeed(s, name, s->report);
}

static void do_theme(scan_t *s, const char *name)
{
    snprintf(s->src, sizeof(s->src), SD_DIR_IMPORT "/%s", name);
    char key[STORAGE_THEME_KEY_LEN];
    esp_err_t err = theme_library_import_file(s->src, key, sizeof(key));
    if (err != ESP_OK) {
        const char *why = "not a theme.bin";
        switch (err) {
        case ESP_ERR_NOT_SUPPORTED: why = "unsupported theme schema_version"; break;
        case ESP_ERR_INVALID_SIZE:  why = "theme.bin truncated or exceeds the 32-bit format limit"; break;
        case ESP_ERR_INVALID_STATE: why = "theme library busy"; break;
        case ESP_ERR_NO_MEM:        why = "out of memory"; break;
        case ESP_ERR_INVALID_RESPONSE: break;
        default:                    why = esp_err_to_name(err); break;
        }
        fail(s, name, why, false);
        return;
    }
    /* the file itself moved into the library: leave a note instead */
    snprintf(s->errtxt, sizeof(s->errtxt), "moved to " SD_DIR_THEMES "/%s/" STORAGE_THEME_FILE "\r\n", key);
    snprintf(s->dst, sizeof(s->dst), SD_DIR_IMPORT "/%s.ok", name);
    sd_card_write_file(s->dst, s->errtxt, strlen(s->errtxt));
    snprintf(s->dst, sizeof(s->dst), SD_DIR_IMPORT "/%s.err.txt", name);
    unlink(s->dst);
    snprintf(s->report, sizeof(s->report), "theme added to library as %s", key);
    log_result(s, true, name, s->report);
}

/* First token of a small text file (UTF-8 BOM and whitespace stripped). */
static esp_err_t read_token(const char *path, char *out, size_t out_len)
{
    char buf[128];
    size_t len = 0;
    out[0] = '\0';
    esp_err_t err = sd_card_read_file(path, buf, sizeof(buf) - 1, &len);
    if (err != ESP_OK) return err;
    buf[len] = '\0';
    char *p = buf;
    if (len >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n])) n++;
    if (n == 0) return ESP_ERR_NOT_FOUND;
    if (n >= out_len) return ESP_ERR_INVALID_SIZE;
    memcpy(out, p, n);
    out[n] = '\0';
    return ESP_OK;
}

static void do_activate(scan_t *s, const char *name)
{
    snprintf(s->src, sizeof(s->src), SD_DIR_IMPORT "/%s", name);
    char id[VPM_ID_LEN];
    esp_err_t err = read_token(s->src, id, sizeof(id));
    if (err != ESP_OK) {
        fail(s, name, err == ESP_ERR_NOT_FOUND ? "empty: write the vehicle profile id on the first line"
                                               : "no valid profile id on the first line", false);
        return;
    }
    err = vp_manager_activate(id);
    if (err != ESP_OK) {
        const char *why = vp_manager_last_error();
        snprintf(s->report, sizeof(s->report), "activate %s failed: %s", id,
                 (why && why[0]) ? why : esp_err_to_name(err));
        fail(s, name, s->report, false);
        return;
    }
    snprintf(s->report, sizeof(s->report), "%s will be active from the next boot", id);
    succeed(s, name, s->report);
}

static void do_reset(scan_t *s, const char *name)
{
    esp_err_t err = vp_manager_reset_to_generic();
    if (err != ESP_OK) {
        snprintf(s->report, sizeof(s->report), "reset failed (%s)", esp_err_to_name(err));
        fail(s, name, s->report, false);
        return;
    }
    first_boot_migration_drop_vehicle();   /* the user's choice beats the migrated one */
    succeed(s, name, VPM_GENERIC_ID " will be active from the next boot");
}

/* ------------------------------------------------------------------ */

static int collect(scan_t *s, kind_t kind)
{
    DIR *dir = opendir(SD_DIR_IMPORT);
    if (!dir) return 0;
    int n = 0;
    struct dirent *e;
    while (n < BATCH && (e = readdir(dir)) != NULL) {
        if (e->d_type != DT_REG || kind_of(e->d_name) != kind) continue;
        if (strlen(e->d_name) >= NAME_LEN - 16) continue;   /* room for ".err.txt" */
        strlcpy(s->names[n++], e->d_name, NAME_LEN);
    }
    closedir(dir);
    return n;
}

static bool process_kind(scan_t *s, kind_t kind)
{
    for (int pass = 0; pass < MAX_PASSES; pass++) {
        int n = collect(s, kind);
        for (int i = 0; i < n; i++) {
            if (!still_allowed()) return false;
            switch (kind) {
            case K_VEHICLE:  do_vehicle(s, s->names[i]); break;
            case K_SIDECAR:  do_sidecar(s, s->names[i]); break;
            case K_THEME:    do_theme(s, s->names[i]); break;
            case K_ACTIVATE: do_activate(s, s->names[i]); break;
            case K_RESET:    do_reset(s, s->names[i]); break;
            default: break;
            }
        }
        if (n < BATCH) break;
    }
    return true;
}

/* Remove *.part left by an interrupted copy in dir (non-recursive). */
static void clean_parts(scan_t *s, const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    int n = 0;
    struct dirent *e;
    while (n < BATCH && (e = readdir(dir)) != NULL) {
        if (e->d_type == DT_REG && storage_has_suffix_ci(e->d_name, ".part") &&
            strlen(e->d_name) < NAME_LEN) {
            strlcpy(s->names[n++], e->d_name, NAME_LEN);
        }
    }
    closedir(dir);
    for (int i = 0; i < n; i++) {
        snprintf(s->src, sizeof(s->src), "%s/%s", dir_path, s->names[i]);
        ESP_LOGI(TAG, "removing stale %s", s->src);
        unlink(s->src);
    }
}

static void clean_theme_parts(scan_t *s)
{
    DIR *dir = opendir(SD_DIR_THEMES);
    if (!dir) return;
    /* collect theme dirs first; clean_parts() reuses names[] */
    char *dirs = storage_psram_alloc((size_t)MAX_DIRS * STORAGE_THEME_KEY_LEN);
    int n = 0;
    struct dirent *e;
    while (dirs && n < MAX_DIRS && (e = readdir(dir)) != NULL) {
        if (e->d_type == DT_DIR && e->d_name[0] != '.' && strlen(e->d_name) < STORAGE_THEME_KEY_LEN) {
            strlcpy(dirs + (size_t)n * STORAGE_THEME_KEY_LEN, e->d_name, STORAGE_THEME_KEY_LEN);
            n++;
        }
    }
    closedir(dir);
    char path[sizeof(SD_DIR_THEMES) + STORAGE_THEME_KEY_LEN + 2];
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), SD_DIR_THEMES "/%s", dirs + (size_t)i * STORAGE_THEME_KEY_LEN);
        clean_parts(s, path);
    }
    if (dirs) heap_caps_free(dirs);
}

static esp_err_t run_scan(bool boot)
{
    if (!sd_card_is_mounted() || ota_wifi_server_is_active() ||
        theme_library_get_state(NULL) == THEME_LIB_WRITING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage_job_lock(LOCK_WAIT)) return ESP_ERR_INVALID_STATE;
    if (!still_allowed() || theme_library_get_state(NULL) == THEME_LIB_WRITING) {
        storage_job_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    scan_t *s = storage_psram_alloc(sizeof(scan_t));
    if (!s) {
        storage_job_unlock();
        return ESP_ERR_NO_MEM;
    }
    memset(s, 0, sizeof(*s));
    sd_card_mkdir(SD_DIR_IMPORT);

    bool ok = true;
    if (!boot) {
        clean_parts(s, SD_DIR_IMPORT);
        clean_parts(s, SD_DIR_BOOT);
        clean_theme_parts(s);
    }
    if (vp_manager_safe_mode()) {
        ESP_LOGW(TAG, "vehicle safe mode: only RESET.TXT is processed");
    } else {
        ok = process_kind(s, K_VEHICLE);
        if (ok && !boot) ok = process_kind(s, K_SIDECAR);
        if (ok && !boot) ok = process_kind(s, K_THEME);
        if (ok) ok = process_kind(s, K_ACTIVATE);
    }
    if (ok) ok = process_kind(s, K_RESET);   /* last: wins over ACTIVATE.TXT */

    if (s->ok || s->err) {
        ESP_LOGI(TAG, "%s scan: %d ok, %d failed", boot ? "boot" : "full", s->ok, s->err);
    }
    heap_caps_free(s);
    storage_job_unlock();
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t sd_import_scan(void)
{
    return run_scan(false);
}

esp_err_t sd_import_scan_boot(void)
{
    return run_scan(true);
}
