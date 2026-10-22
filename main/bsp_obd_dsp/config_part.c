/*
 * config partition - LittleFS (label "config", 1 MB) mounted at /config.
 *
 * - Never formats on its own: config_part_init() reports a corrupt FS and
 *   the caller decides (format + restore from the SD mirror).
 * - Writes are atomic: "<file>.tmp" + fsync + rename (LittleFS rename
 *   replaces the destination atomically).
 * - Quota: CONFIG_QUOTA_PCT of the partition; a write that would grow usage
 *   past it is rejected with ESP_ERR_NO_MEM.
 * - SD mirror: every write/delete/rename is repeated under SD /OBD/CFG when a
 *   card is mounted (best effort; never fails the flash write).
 */

#include "config_part.h"
#include "sd_card.h"

#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "config_part"

#define CFG_PATH_MAX      200
#define CFG_BLOCK         4096u              /* LittleFS block = flash sector */
#define CFG_FILE_MAX      (256u * 1024u)     /* largest file mirrored/restored */
#define TMP_SUFFIX        ".tmp"

static bool              s_mounted;
static SemaphoreHandle_t s_lock;

static void cfg_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void cfg_unlock(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

static bool has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

/* "/config/x", "/x" or "x" -> "/config/x". Rejects ".." and overlong paths. */
static bool full_path(const char *path, char *out, size_t n)
{
    if (!path || !path[0]) return false;
    const size_t mp = strlen(CONFIG_MOUNT_POINT);
    int r;
    if (strncmp(path, CONFIG_MOUNT_POINT, mp) == 0 && (path[mp] == '/' || path[mp] == '\0')) {
        r = snprintf(out, n, "%s", path);
    } else if (path[0] == '/') {
        r = snprintf(out, n, "%s%s", CONFIG_MOUNT_POINT, path);
    } else {
        r = snprintf(out, n, "%s/%s", CONFIG_MOUNT_POINT, path);
    }
    if (r <= 0 || (size_t)r >= n) return false;
    return strstr(out, "/..") == NULL;
}

/* "/config/x/y" -> "/sdcard/OBD/CFG/x/y" */
static bool mirror_path(const char *full, char *out, size_t n)
{
    const char *rel = full + strlen(CONFIG_MOUNT_POINT);
    int r = snprintf(out, n, "%s%s", SD_DIR_CFG, rel);
    return r > 0 && (size_t)r < n;
}

static void lfs_mkdir_parents(const char *full)
{
    char tmp[CFG_PATH_MAX];
    strlcpy(tmp, full, sizeof(tmp));
    for (char *p = tmp + strlen(CONFIG_MOUNT_POINT) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
}

static void sd_mkdir_parents(const char *sd_path)
{
    char tmp[CFG_PATH_MAX];
    strlcpy(tmp, sd_path, sizeof(tmp));
    for (char *p = tmp + strlen(SD_MOUNT_POINT) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            sd_card_mkdir(tmp);
            *p = '/';
        }
    }
}

static size_t round_blk(size_t n)
{
    return ((n + CFG_BLOCK - 1) / CFG_BLOCK) * CFG_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Raw file I/O on LittleFS                                            */
/* ------------------------------------------------------------------ */

static esp_err_t lfs_write_atomic(const char *full, const void *data, size_t len)
{
    char tmp[CFG_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s" TMP_SUFFIX, full);
    lfs_mkdir_parents(full);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s: errno %d", tmp, errno);
        return ESP_FAIL;
    }
    size_t put = len ? fwrite(data, 1, len, f) : 0;
    bool ok = (put == len) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        ESP_LOGE(TAG, "short write %s (%u/%u)", tmp, (unsigned)put, (unsigned)len);
        unlink(tmp);
        return ESP_FAIL;
    }
    if (rename(tmp, full) != 0) {
        ESP_LOGE(TAG, "commit %s: errno %d", full, errno);
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Read a whole LittleFS file into a fresh PSRAM buffer. */
static esp_err_t lfs_read_alloc(const char *full, size_t max_len, uint8_t **out, size_t *out_len)
{
    struct stat st;
    if (stat(full, &st) != 0) return ESP_ERR_NOT_FOUND;
    size_t sz = (size_t)st.st_size;
    if (sz > max_len) return ESP_ERR_INVALID_SIZE;
    uint8_t *buf = heap_caps_malloc(sz ? sz : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(sz ? sz : 1, MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;
    FILE *f = fopen(full, "rb");
    if (!f) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    size_t got = sz ? fread(buf, 1, sz, f) : 0;
    fclose(f);
    if (got != sz) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    *out = buf;
    *out_len = sz;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* SD mirror (best effort)                                             */
/* ------------------------------------------------------------------ */

static bool sd_ready(void)
{
    return sd_card_get_state() == SD_STATE_MOUNTED;
}

/* Write one file's mirror unless the SD copy is already identical. */
static void mirror_data(const char *full, const void *data, size_t len)
{
    char sd[CFG_PATH_MAX];
    if (!sd_ready() || !mirror_path(full, sd, sizeof(sd))) return;

    struct stat st;
    if (stat(sd, &st) == 0 && (size_t)st.st_size == len) {
        void *old = NULL;
        size_t old_len = 0;
        if (sd_card_read_file_alloc(sd, len, &old, &old_len) == ESP_OK) {
            bool same = (old_len == len) && memcmp(old, data, len) == 0;
            heap_caps_free(old);
            if (same) return;
        }
    }
    sd_mkdir_parents(sd);
    esp_err_t err = sd_card_write_file_atomic(sd, data, len);
    if (err != ESP_OK) ESP_LOGW(TAG, "SD mirror of %s failed: %s", full, esp_err_to_name(err));
}

static void mirror_file(const char *full)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    if (lfs_read_alloc(full, CFG_FILE_MAX, &buf, &len) != ESP_OK) return;
    mirror_data(full, buf, len);
    heap_caps_free(buf);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t config_part_init(void)
{
    if (s_mounted) return ESP_ERR_INVALID_STATE;
    cfg_lock();

    esp_vfs_littlefs_conf_t conf = {
        .base_path = CONFIG_MOUNT_POINT,
        .partition_label = CONFIG_PART_LABEL,
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_OK) {
        s_mounted = true;
        size_t used = 0, total = 0;
        esp_littlefs_info(CONFIG_PART_LABEL, &total, &used);
        ESP_LOGI(TAG, "mounted %s: %u/%u bytes used", CONFIG_MOUNT_POINT, (unsigned)used, (unsigned)total);
    } else {
        /* A failed register leaves nothing mounted; make sure the VFS slot is free. */
        esp_vfs_littlefs_unregister(CONFIG_PART_LABEL);
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
    }
    cfg_unlock();
    return err;
}

esp_err_t config_part_format(void)
{
    cfg_lock();
    if (s_mounted) {
        esp_vfs_littlefs_unregister(CONFIG_PART_LABEL);
        s_mounted = false;
    }
    esp_err_t err = esp_littlefs_format(CONFIG_PART_LABEL);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "config partition formatted");
    } else {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
    }
    cfg_unlock();
    return err;
}

bool config_part_is_mounted(void) { return s_mounted; }

esp_err_t config_part_get_usage(size_t *used_bytes, size_t *total_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    size_t total = 0, used = 0;
    esp_err_t err = esp_littlefs_info(CONFIG_PART_LABEL, &total, &used);
    if (err != ESP_OK) return err;
    if (used_bytes)  *used_bytes  = used;
    if (total_bytes) *total_bytes = (size_t)((uint64_t)total * CONFIG_QUOTA_PCT / 100);
    return ESP_OK;
}

bool config_part_has_quota(size_t needed_bytes)
{
    size_t used = 0, quota = 0;
    if (config_part_get_usage(&used, &quota) != ESP_OK) return false;
    return used + round_blk(needed_bytes) <= quota;
}

/* Net growth of replacing `full` with len bytes must fit in the quota. */
static bool quota_ok(const char *full, size_t len)
{
    size_t used = 0, quota = 0;
    if (config_part_get_usage(&used, &quota) != ESP_OK) return false;
    size_t need = round_blk(len) + CFG_BLOCK;           /* data + metadata pair */
    size_t freed = 0;
    struct stat st;
    if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) freed = round_blk((size_t)st.st_size) + CFG_BLOCK;
    if (need <= freed) return true;
    return used + (need - freed) <= quota;
}

esp_err_t config_part_write(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len > 0)) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    char full[CFG_PATH_MAX];
    if (!full_path(path, full, sizeof(full))) return ESP_ERR_INVALID_ARG;

    cfg_lock();
    esp_err_t err;
    if (!quota_ok(full, len)) {
        ESP_LOGW(TAG, "quota (%d%%) exceeded, rejecting %s (%u bytes)", CONFIG_QUOTA_PCT, full, (unsigned)len);
        err = ESP_ERR_NO_MEM;
    } else {
        err = lfs_write_atomic(full, data, len);
        if (err == ESP_OK) mirror_data(full, data, len);
    }
    cfg_unlock();
    return err;
}

esp_err_t config_part_read(const char *path, void *buf, size_t buf_len, size_t *bytes_read)
{
    if (bytes_read) *bytes_read = 0;
    if (!path || (!buf && buf_len)) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    char full[CFG_PATH_MAX];
    if (!full_path(path, full, sizeof(full))) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(full, "rb");
    if (!f) return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    size_t got = buf_len ? fread(buf, 1, buf_len, f) : 0;
    /* Truncation is an error: the caller's buffer was too small. */
    bool more = (fgetc(f) != EOF);
    fclose(f);
    if (bytes_read) *bytes_read = got;
    return more ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t config_part_remove(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    char full[CFG_PATH_MAX];
    if (!full_path(path, full, sizeof(full))) return ESP_ERR_INVALID_ARG;

    cfg_lock();
    esp_err_t err = ESP_OK;
    if (unlink(full) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "unlink %s: errno %d", full, errno);
        err = ESP_FAIL;
    }
    if (err == ESP_OK && sd_ready()) {
        char sd[CFG_PATH_MAX];
        if (mirror_path(full, sd, sizeof(sd))) sd_card_remove(sd);
    }
    cfg_unlock();
    return err;
}

bool config_part_exists(const char *path)
{
    if (!s_mounted || !path) return false;
    char full[CFG_PATH_MAX];
    if (!full_path(path, full, sizeof(full))) return false;
    struct stat st;
    return stat(full, &st) == 0;
}

/* ------------------------------------------------------------------ */
/* Whole-tree mirror / restore                                         */
/* ------------------------------------------------------------------ */

static void mirror_dir(const char *lfs_dir)
{
    DIR *d = opendir(lfs_dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' || has_suffix(ent->d_name, TMP_SUFFIX)) continue;
        char p[CFG_PATH_MAX];
        if (snprintf(p, sizeof(p), "%s/%s", lfs_dir, ent->d_name) >= (int)sizeof(p)) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            mirror_dir(p);
        } else {
            mirror_file(p);
        }
    }
    closedir(d);
}

void config_part_mirror_to_sd(void)
{
    if (!s_mounted || !sd_ready()) return;
    cfg_lock();
    sd_card_mkdir(SD_DIR_CFG);
    mirror_dir(CONFIG_MOUNT_POINT);
    cfg_unlock();
    ESP_LOGI(TAG, "config mirrored to %s", SD_DIR_CFG);
}

static int restore_dir(const char *sd_dir, int *n_ok)
{
    DIR *d = opendir(sd_dir);
    if (!d) return 0;
    int n_fail = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        if (nm[0] == '.' || has_suffix(nm, ".part") || has_suffix(nm, TMP_SUFFIX)) continue;
        char sd[CFG_PATH_MAX];
        if (snprintf(sd, sizeof(sd), "%s/%s", sd_dir, nm) >= (int)sizeof(sd)) continue;
        struct stat st;
        if (stat(sd, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            n_fail += restore_dir(sd, n_ok);
            continue;
        }
        char full[CFG_PATH_MAX];
        if (snprintf(full, sizeof(full), "%s%s", CONFIG_MOUNT_POINT,
                     sd + strlen(SD_DIR_CFG)) >= (int)sizeof(full)) {
            continue;
        }
        void *buf = NULL;
        size_t len = 0;
        esp_err_t err = sd_card_read_file_alloc(sd, CFG_FILE_MAX, &buf, &len);
        if (err == ESP_OK) {
            err = quota_ok(full, len) ? lfs_write_atomic(full, buf, len) : ESP_ERR_NO_MEM;
            heap_caps_free(buf);
        }
        if (err == ESP_OK) {
            (*n_ok)++;
        } else {
            ESP_LOGW(TAG, "restore %s: %s", sd, esp_err_to_name(err));
            n_fail++;
        }
    }
    closedir(d);
    return n_fail;
}

esp_err_t config_part_restore_from_sd(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!sd_ready()) {
        ESP_LOGW(TAG, "SD not mounted, cannot restore config");
        return ESP_ERR_NOT_FOUND;
    }
    if (!sd_card_exists(SD_DIR_CFG)) {
        ESP_LOGI(TAG, "no SD mirror at %s", SD_DIR_CFG);
        return ESP_OK;
    }
    cfg_lock();
    int n_ok = 0;
    int n_fail = restore_dir(SD_DIR_CFG, &n_ok);
    cfg_unlock();
    ESP_LOGI(TAG, "restored %d file(s) from %s, %d failed", n_ok, SD_DIR_CFG, n_fail);
    return n_fail ? ESP_FAIL : ESP_OK;
}
