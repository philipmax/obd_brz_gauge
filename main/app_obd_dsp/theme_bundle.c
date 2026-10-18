/* USB theme installation: stream into an isolated directory, validate, then
 * swap with a recovery journal. Never change the selected theme on bad input. */
#include "app_obd_dsp/theme_bundle.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/theme_library.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "bsp_obd_dsp/nvs_app.h"
#include "export_path/ui_ext.h"
#include "theme_engine/theme_sidecar_core.h"
#include "theme_engine/theme_interface.h"

#define STAGE SD_DIR_IMPORT "/.usb-theme-stage"
#define BACKUP SD_DIR_IMPORT "/.usb-theme-backup"
#define JOURNAL SD_DIR_IMPORT "/.usb-theme-journal"
#define MAX_FILES 128
#define CHUNK 4096
static const char *TAG = "theme_bundle";
extern SemaphoreHandle_t lvgl_mux;

typedef struct {
    char magic[4];
    char key[65];
    char previous[65];
    uint8_t had_old;
} journal_t;

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static bool safe_key(const char *p) {
    if (!p[0] || !strcmp(p, ".") || !strcmp(p, "..")) return false;
    for (; *p; ++p) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
    return true;
}
static bool remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) return errno == ENOENT;
    bool ok = true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[384];
        int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { ok = false; break; }
        struct stat st;
        if (stat(child, &st) != 0 || (S_ISDIR(st.st_mode) ? !remove_tree(child) : unlink(child) != 0)) ok = false;
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}
static bool read_bytes(FILE *f, void *buf, size_t n) { return sd_card_read(f, buf, n) == n; }
static void *alloc_core(size_t n, void *ctx) { (void)ctx; return storage_psram_alloc(n); }
static void free_core(void *p, void *ctx) { (void)ctx; free(p); }

/* Caller owns storage lock. Journal remains on any recovery failure. */
static esp_err_t recover_locked(void) {
    if (!exists(JOURNAL)) {
        remove_tree(STAGE);
        remove_tree(BACKUP); /* only obsolete backups after a committed install */
        sd_card_remove(THEME_BUNDLE_PATH);
        sd_card_remove(THEME_BUNDLE_PATH ".part");
        return ESP_OK;
    }
    journal_t j;
    size_t got = 0;
    if (sd_card_read_file(JOURNAL, &j, sizeof(j), &got) != ESP_OK || got != sizeof(j) ||
        memcmp(j.magic, "THJ1", 4) || j.key[64] || j.previous[64] || !safe_key(j.key) ||
        (j.previous[0] && !safe_key(j.previous)) || j.had_old > 1) return ESP_ERR_INVALID_RESPONSE;
    char target[160];
    snprintf(target, sizeof(target), SD_DIR_THEMES "/%s", j.key);
    if (exists(BACKUP)) {
        if (exists(target) && !remove_tree(target)) return ESP_FAIL;
        if (rename(BACKUP, target) != 0) return ESP_FAIL;
    } else if (!j.had_old && !exists(STAGE) && exists(target)) {
        if (!remove_tree(target)) return ESP_FAIL;
    }
    esp_err_t err = j.previous[0] ? nvs_app_set_str("imp", "th_cur", j.previous)
                                  : nvs_app_erase_key("imp", "th_cur");
    if (err != ESP_OK) return err;
    if (sd_card_remove(JOURNAL) != ESP_OK) return ESP_FAIL;
    remove_tree(STAGE);
    sd_card_remove(THEME_BUNDLE_PATH);
    sd_card_remove(THEME_BUNDLE_PATH ".part");
    ESP_LOGW(TAG, "interrupted installation rolled back");
    return ESP_OK;
}

esp_err_t theme_bundle_recover(void) {
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    esp_err_t err = recover_locked();
    storage_job_unlock();
    return err;
}

/* Staging writes never touch live theme files. */
static esp_err_t unpack(const char *path, char key[65]) {
    FILE *in = fopen(path, "rb");
    if (!in) return ESP_ERR_NOT_FOUND;
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    char (*names)[69] = storage_psram_alloc(MAX_FILES * 69);
    tsc_alloc_t allocator = {alloc_core, free_core, NULL};
    tsc_builder_t *builder = tsc_builder_new(&allocator);
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    bool have_bin = false, have_project = false;
    uint32_t project_size = 0;
    uint8_t header[8];
    if (!buf || !names || !builder) { err = ESP_ERR_NO_MEM; goto done; }
    if (!read_bytes(in, header, 8) || memcmp(header, "OBDT\1\0", 6)) goto done;
    unsigned count = u16(header + 6);
    if (count < 2 || count > MAX_FILES) goto done;
    if (sd_card_mkdir(SD_DIR_THEMES) != ESP_OK) { err = ESP_FAIL; goto done; }
    if (!remove_tree(STAGE) || sd_card_mkdir(STAGE) != ESP_OK) { err = ESP_FAIL; goto done; }
    for (unsigned i = 0; i < count; ++i) {
        if (!read_bytes(in, header, 6)) goto done;
        unsigned name_len = u16(header);
        uint32_t bytes = u32(header + 2);
        if (!name_len || name_len > 68 || !bytes || !read_bytes(in, names[i], name_len)) goto done;
        names[i][name_len] = 0;
        if (strlen(names[i]) != name_len) goto done;
        for (unsigned j = 0; j < i; ++j) if (!strcasecmp(names[i], names[j])) goto done;
        bool bin = !strcmp(names[i], "theme.bin");
        bool project = !strcmp(names[i], "project.json");
        if (!bin && !project) {
            char *dot = strchr(names[i], '.');
            if (!dot || strcasecmp(dot, ".obv") || dot == names[i] || dot - names[i] > 64) goto done;
            for (char *c = names[i]; c < dot; ++c) if (!((*c >= 'a' && *c <= 'z') ||
                (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) goto done;
        }
        if (bin && bytes < STORAGE_THEME_MANIFEST_SIZE) goto done;
        if (project && bytes > 32u * 1024u * 1024u) goto done;
        char dest[160];
        snprintf(dest, sizeof(dest), STAGE "/%s", names[i]);
        FILE *out = project ? NULL : fopen(dest, "wb");
        if (!project && !out) { err = ESP_FAIL; goto done; }
        bool ok = true;
        for (uint32_t left = bytes; left;) {
            size_t n = left > CHUNK ? CHUNK : left;
            if (!read_bytes(in, buf, n) || (project ? !tsc_builder_feed(builder, (char *)buf, n)
                                                  : sd_card_write(out, buf, n) != n)) { ok = false; break; }
            left -= n;
        }
        if (out) {
            ok = (fflush(out) == 0) && ok;
            ok = (fsync(fileno(out)) == 0) && ok;
            ok = (fclose(out) == 0) && ok;
        }
        if (!ok) goto done;
        have_bin |= bin;
        have_project |= project;
        if (project) project_size = bytes;
    }
    if (!have_bin || !have_project || fgetc(in) != EOF || ferror(in)) goto done;
    storage_theme_meta_t meta;
    if (storage_theme_meta_from_file(STAGE "/theme.bin", &meta) != ESP_OK || !tsc_valid_theme_id(meta.id)) goto done;
    char *sidecar = NULL;
    size_t sidecar_len = 0;
    tsc_stats_t stats;
    tsc_err_t result = tsc_builder_finish(builder, project_size, NULL, &sidecar, &sidecar_len, &stats);
    if (result == TSC_OK && !strcmp(stats.theme_id, meta.id) && !stats.pages_invalid && !stats.pages_too_big) {
        err = sd_card_write_file_atomic(STAGE "/sidecar.json", sidecar, sidecar_len);
        storage_theme_key(meta.id, key, 65);
    }
    free(sidecar);
done:
    tsc_builder_free(builder);
    free(names);
    free(buf);
    fclose(in);
    if (err != ESP_OK) remove_tree(STAGE);
    return err;
}

esp_err_t theme_bundle_install(const char *path) {
    if (ota_wifi_server_is_active() || !sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    /* A leftover journal must be recovered at boot before another install. */
    char key[65] = {0};
    esp_err_t err = exists(JOURNAL) ? ESP_ERR_INVALID_STATE : unpack(path, key);
    storage_job_unlock();
    if (err != ESP_OK) return err;

    /* LVGL -> storage is the same order as theme reads. Never wait for LVGL
     * while owning storage, since a rendering task may already be waiting. */
    if (!lvgl_mux || xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) { xSemaphoreGive(lvgl_mux); return ESP_ERR_TIMEOUT; }
    err = ui_ext_theme_prepare_install();
    if (err == ESP_OK) {
        char target[160];
        snprintf(target, sizeof(target), SD_DIR_THEMES "/%s", key);
        journal_t journal = { .magic = {'T','H','J','1'} };
        strlcpy(journal.key, key, sizeof(journal.key));
        theme_library_current_key(journal.previous, sizeof(journal.previous));
        journal.had_old = exists(target);
        if (!remove_tree(BACKUP)) err = ESP_FAIL;
        if (err == ESP_OK) err = sd_card_write_file_atomic(JOURNAL, &journal, sizeof(journal));
        if (err == ESP_OK && journal.had_old && rename(target, BACKUP) != 0) err = ESP_FAIL;
        if (err == ESP_OK && rename(STAGE, target) != 0) err = ESP_FAIL;
        if (err == ESP_OK) err = theme_library_select(key);
        if (err == ESP_OK) err = sd_card_remove(JOURNAL); /* commit point */
        if (err == ESP_OK) {
            remove_tree(BACKUP);
            sd_card_remove(path);
            ESP_LOGI(TAG, "installed and selected %s", key);
        } else if (exists(JOURNAL)) {
            esp_err_t rollback = recover_locked();
            if (rollback != ESP_OK) ESP_LOGE(TAG, "rollback pending: %s", esp_err_to_name(rollback));
        }
        if (err != ESP_OK) theme_load(0);
    }
    storage_job_unlock();
    xSemaphoreGive(lvgl_mux);
    return err;
}
