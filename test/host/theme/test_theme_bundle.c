#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "app_obd_dsp/theme_bundle.h"
#include "app_obd_dsp/theme_library.h"
#include "cJSON.h"

#define STAGE SD_DIR_IMPORT "/.usb-theme-stage"
#define BACKUP SD_DIR_IMPORT "/.usb-theme-backup"
#define JOURNAL SD_DIR_IMPORT "/.usb-theme-journal"
#define LIVE SD_DIR_THEMES "/DemoTheme"
static char selected[65] = "DemoTheme";
static bool fail_select, fail_ui, fail_lvgl;
static unsigned storage_depth, ui_depth, ui_calls, reload_calls;
SemaphoreHandle_t lvgl_mux = (void *)1;

#if !defined(__APPLE__)
size_t strlcpy(char *out, const char *in, size_t n) {
    size_t len = strlen(in);
    if (n) { size_t use = len < n-1 ? len : n-1; memcpy(out, in, use); out[use] = 0; }
    return len;
}
#endif
void storage_test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test"; }
bool storage_job_lock(TickType_t wait) { (void)wait; ++storage_depth; return true; }
void storage_job_unlock(void) { assert(storage_depth); --storage_depth; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mux, TickType_t wait) {
    (void)mux; (void)wait; assert(storage_depth == 0); if (fail_lvgl) return 0; ++ui_depth; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mux) { (void)mux; assert(ui_depth == 1); --ui_depth; return pdTRUE; }
bool sd_card_is_mounted(void) { return true; }
bool ota_wifi_server_is_active(void) { return false; }
void *storage_psram_alloc(size_t n) { return malloc(n); }
size_t sd_card_read(FILE *f, void *buf, size_t n) { return fread(buf, 1, n, f); }
size_t sd_card_write(FILE *f, const void *buf, size_t n) { return fwrite(buf, 1, n, f); }
esp_err_t sd_card_mkdir(const char *p) { return mkdir(p, 0700) == 0 || errno == EEXIST ? ESP_OK : ESP_FAIL; }
esp_err_t sd_card_remove(const char *p) { return unlink(p) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL; }
esp_err_t sd_card_read_file(const char *p, void *buf, size_t cap, size_t *got) {
    FILE *f = fopen(p, "rb"); if (!f) return ESP_FAIL; *got = fread(buf, 1, cap, f); fclose(f); return ESP_OK;
}
esp_err_t sd_card_write_file_atomic(const char *p, const void *buf, size_t n) {
    char tmp[256]; snprintf(tmp, sizeof(tmp), "%s.part", p);
    FILE *f = fopen(tmp, "wb"); if (!f) return ESP_FAIL;
    bool ok = fwrite(buf, 1, n, f) == n; ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp, p) == 0 ? ESP_OK : ESP_FAIL;
}
void storage_theme_key(const char *id, char *out, size_t n) { strlcpy(out, id, n); }
esp_err_t storage_theme_meta_from_file(const char *p, storage_theme_meta_t *meta) {
    char buf[16385]; size_t n = 0;
    if (sd_card_read_file(p, buf, 16384, &n) != ESP_OK || n != 16384) return ESP_FAIL;
    buf[16384] = 0;
    cJSON *j = cJSON_Parse(buf), *theme = cJSON_GetObjectItem(j, "theme");
    cJSON *id = cJSON_GetObjectItem(theme, "id");
    if (!cJSON_IsString(id)) { cJSON_Delete(j); return ESP_FAIL; }
    memset(meta, 0, sizeof(*meta)); strlcpy(meta->id, id->valuestring, sizeof(meta->id)); cJSON_Delete(j); return ESP_OK;
}
void theme_library_current_key(char *out, size_t n) { strlcpy(out, selected, n); }
esp_err_t theme_library_select(const char *key) {
    assert(ui_depth == 1 && storage_depth > 0);
    if (fail_select) return ESP_FAIL;
    strlcpy(selected, key, sizeof(selected)); return ESP_OK;
}
esp_err_t nvs_app_set_str(const char *ns, const char *key, const char *v) {
    (void)ns; (void)key; strlcpy(selected, v, sizeof(selected)); return ESP_OK;
}
esp_err_t nvs_app_erase_key(const char *ns, const char *key) { (void)ns; (void)key; selected[0] = 0; return ESP_OK; }
esp_err_t ui_ext_theme_prepare_install(void) { assert(ui_depth == 1); ++ui_calls; return fail_ui ? ESP_FAIL : ESP_OK; }
esp_err_t theme_load(uint8_t slot) { (void)slot; ++reload_calls; return ESP_OK; }

static void put16(FILE *f, size_t n) { fputc((int)(n & 255), f); fputc((int)((n >> 8) & 255), f); }
static void put32(FILE *f, size_t n) { put16(f, n); put16(f, n >> 16); }
static void record_file(FILE *f, const char *name, const void *bytes, size_t n) {
    put16(f, strlen(name)); put32(f, n); fwrite(name, 1, strlen(name), f); fwrite(bytes, 1, n, f);
}
static void bundle(const char *theme_id, const char *project_id, const char *video_name) {
    FILE *f = fopen(THEME_BUNDLE_PATH, "wb"); assert(f);
    fwrite("OBDT\1\0", 1, 6, f); put16(f, video_name ? 3 : 2);
    char bin[16384] = {0}; snprintf(bin, sizeof(bin), "{\"schema_version\":\"1.0\",\"theme\":{\"id\":\"%s\"}}", theme_id);
    record_file(f, "theme.bin", bin, sizeof(bin));
    char project[256]; snprintf(project, sizeof(project), "{\"schemaVersion\":\"1.0\",\"meta\":{\"id\":\"%s\"},\"assets\":[],\"pages\":[{\"id\":\"p\",\"elements\":[]}]}", project_id);
    record_file(f, "project.json", project, strlen(project));
    if (video_name) record_file(f, video_name, "video", 5);
    assert(fclose(f) == 0);
}
static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void write_old(void) { assert(sd_card_write_file_atomic(LIVE "/theme.bin", "old", 3) == ESP_OK); }
static void assert_old(void) { char b[8]; size_t n; assert(sd_card_read_file(LIVE "/theme.bin", b, sizeof(b), &n) == ESP_OK); assert(n == 3 && !memcmp(b, "old", 3)); }
static void journal(bool had_old, const char *previous) {
    struct { char magic[4], key[65], previous[65]; uint8_t had_old; } j = {.magic={'T','H','J','1'}};
    strcpy(j.key, "DemoTheme"); strlcpy(j.previous, previous, 65); j.had_old = had_old;
    assert(sd_card_write_file_atomic(JOURNAL, &j, sizeof(j)) == ESP_OK);
}
int main(void) {
    char dir[] = "/tmp/obd-theme-bundle-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    assert(sd_card_mkdir("OBD") == ESP_OK && sd_card_mkdir(SD_DIR_IMPORT) == ESP_OK && sd_card_mkdir(SD_DIR_THEMES) == ESP_OK);
    assert(sd_card_mkdir(LIVE) == ESP_OK); write_old();
    bundle("DemoTheme", "WrongTheme", NULL);
    assert(theme_bundle_install(THEME_BUNDLE_PATH) != ESP_OK); assert_old(); assert(ui_calls == 0);
    bundle("DemoTheme", "DemoTheme", "../escape.obv");
    assert(theme_bundle_install(THEME_BUNDLE_PATH) != ESP_OK); assert_old(); assert(!exists("OBD/IMPORT/escape.obv"));
    bundle("DemoTheme", "DemoTheme", NULL); fail_select = true;
    assert(theme_bundle_install(THEME_BUNDLE_PATH) != ESP_OK); fail_select = false;
    assert_old(); assert(!strcmp(selected, "DemoTheme") && !exists(JOURNAL) && reload_calls == 1);
    bundle("DemoTheme", "DemoTheme", NULL); fail_ui = true;
    assert(theme_bundle_install(THEME_BUNDLE_PATH) != ESP_OK); fail_ui = false; assert_old();
    fail_lvgl = true;
    assert(theme_bundle_install(THEME_BUNDLE_PATH) != ESP_OK); fail_lvgl = false; assert_old();
    bundle("DemoTheme", "DemoTheme", "rev.obv");
    assert(theme_bundle_install(THEME_BUNDLE_PATH) == ESP_OK);
    assert(exists(LIVE "/theme.bin") && exists(LIVE "/sidecar.json") && exists(LIVE "/rev.obv"));
    assert(!exists(STAGE) && !exists(BACKUP) && !exists(JOURNAL) && !exists(THEME_BUNDLE_PATH));
    char sidecar[4096]; size_t n;
    assert(sd_card_read_file(LIVE "/sidecar.json", sidecar, sizeof(sidecar)-1, &n) == ESP_OK); sidecar[n] = 0;
    assert(strstr(sidecar, "gauge-sidecar/1") && strstr(sidecar, "DemoTheme"));
    /* Successful overwrite replaces old videos together with the theme. */
    bundle("DemoTheme", "DemoTheme", NULL); assert(theme_bundle_install(THEME_BUNDLE_PATH) == ESP_OK);
    assert(!exists(LIVE "/rev.obv"));
    /* Power interruption after stage -> live, before the journal is removed. */
    journal(true, "PreviousTheme"); assert(rename(LIVE, BACKUP) == 0); assert(sd_card_mkdir(LIVE) == ESP_OK); write_old();
    assert(theme_bundle_recover() == ESP_OK); assert(!strcmp(selected, "PreviousTheme"));
    assert(exists(LIVE "/sidecar.json") && !exists(JOURNAL) && !exists(BACKUP));
    /* Interruption installing a previously absent ID restores built-in selection. */
    journal(false, ""); assert(theme_bundle_recover() == ESP_OK);
    assert(!exists(LIVE) && !selected[0]);
    assert(storage_depth == 0 && ui_depth == 0);
    assert(rmdir(SD_DIR_THEMES) == 0 && rmdir(SD_DIR_IMPORT) == 0 && rmdir("OBD") == 0);
    assert(chdir("/") == 0 && rmdir(dir) == 0);
    puts("theme bundle install/recovery passed");
    return 0;
}
