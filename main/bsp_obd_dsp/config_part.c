#include "config_part.h"
#include "sd_card.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <errno.h>

#define TAG "config_part"

/* SD mirror root for config partition */
#define SD_MIRROR_ROOT    (SD_MOUNT_POINT "/OBD/CFG")

static bool s_mounted = false;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Create intermediate directories for path (modifies a copy). */
static void mkdir_parents(const char *path)
{
    char tmp[512];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* Map a config-partition absolute path to its SD mirror equivalent.
 * E.g. "/config/themes/foo" → "/sdcard/OBD/CFG/themes/foo" */
static void mirror_path(const char *src, char *dst, size_t dst_len)
{
    const char *rel = src;
    /* Strip /config prefix if present */
    if (strncmp(rel, CONFIG_MOUNT_POINT, strlen(CONFIG_MOUNT_POINT)) == 0) {
        rel += strlen(CONFIG_MOUNT_POINT);
    }
    snprintf(dst, dst_len, "%s%s", SD_MIRROR_ROOT, rel);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t config_part_init(void)
{
    if (s_mounted) return ESP_ERR_INVALID_STATE;

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
        ESP_LOGI(TAG, "Mounted /config: %u/%u bytes used", (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGW(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_part_deinit(void)
{
    if (!s_mounted) return ESP_OK;
    esp_err_t err = esp_vfs_littlefs_unregister(CONFIG_PART_LABEL);
    s_mounted = false;
    return err;
}

esp_err_t config_part_format(void)
{
    /* Must be unmounted before formatting */
    if (s_mounted) {
        esp_vfs_littlefs_unregister(CONFIG_PART_LABEL);
        s_mounted = false;
    }
    esp_err_t err = esp_littlefs_format(CONFIG_PART_LABEL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "config partition formatted");
    } else {
        ESP_LOGE(TAG, "config format failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool config_part_is_mounted(void) { return s_mounted; }

esp_err_t config_part_get_usage(size_t *used_bytes, size_t *total_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    size_t total = 0, used = 0;
    esp_err_t err = esp_littlefs_info(CONFIG_PART_LABEL, &total, &used);
    if (err != ESP_OK) return err;

    /* Cap total at quota */
    size_t quota = (size_t)((uint64_t)total * CONFIG_QUOTA_PCT / 100);
    if (used_bytes)  *used_bytes  = used;
    if (total_bytes) *total_bytes = quota;
    return ESP_OK;
}

bool config_part_has_quota(size_t needed_bytes)
{
    size_t used = 0, quota = 0;
    if (config_part_get_usage(&used, &quota) != ESP_OK) return false;
    return (used + needed_bytes) <= quota;
}

esp_err_t config_part_write(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len > 0)) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!config_part_has_quota(len)) {
        ESP_LOGW(TAG, "config quota exceeded, rejecting write to %s (%u bytes)", path, (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    /* Build the full path under the mount point */
    char full[512];
    if (path[0] != '/') {
        snprintf(full, sizeof(full), "%s/%s", CONFIG_MOUNT_POINT, path);
    } else if (strncmp(path, CONFIG_MOUNT_POINT, strlen(CONFIG_MOUNT_POINT)) == 0) {
        strlcpy(full, path, sizeof(full));
    } else {
        snprintf(full, sizeof(full), "%s%s", CONFIG_MOUNT_POINT, path);
    }

    mkdir_parents(full);

    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for write: errno %d", full, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Short write on %s: %u/%u", full, (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }

    /* Best-effort SD mirror */
    if (sd_card_get_state() == SD_STATE_MOUNTED) {
        char mirror[512];
        mirror_path(full, mirror, sizeof(mirror));
        mkdir_parents(mirror);
        esp_err_t sd_err = sd_card_write_file(mirror, data, len);
        if (sd_err != ESP_OK) {
            ESP_LOGW(TAG, "SD mirror of %s failed: %s", full, esp_err_to_name(sd_err));
        }
    }

    return ESP_OK;
}

esp_err_t config_part_read(const char *path, void *buf, size_t buf_len, size_t *bytes_read)
{
    if (!path || !buf) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    char full[512];
    if (path[0] != '/') {
        snprintf(full, sizeof(full), "%s/%s", CONFIG_MOUNT_POINT, path);
    } else if (strncmp(path, CONFIG_MOUNT_POINT, strlen(CONFIG_MOUNT_POINT)) == 0) {
        strlcpy(full, path, sizeof(full));
    } else {
        snprintf(full, sizeof(full), "%s%s", CONFIG_MOUNT_POINT, path);
    }

    FILE *f = fopen(full, "rb");
    if (!f) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    size_t got = fread(buf, 1, buf_len, f);
    fclose(f);

    if (bytes_read) *bytes_read = got;
    return ESP_OK;
}

esp_err_t config_part_remove(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    char full[512];
    if (strncmp(path, CONFIG_MOUNT_POINT, strlen(CONFIG_MOUNT_POINT)) == 0) {
        strlcpy(full, path, sizeof(full));
    } else if (path[0] == '/') {
        snprintf(full, sizeof(full), "%s%s", CONFIG_MOUNT_POINT, path);
    } else {
        snprintf(full, sizeof(full), "%s/%s", CONFIG_MOUNT_POINT, path);
    }

    int rc = unlink(full);
    if (rc != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "unlink %s failed: errno %d", full, errno);
        return ESP_FAIL;
    }

    /* Best-effort SD mirror removal */
    if (sd_card_get_state() == SD_STATE_MOUNTED) {
        char mirror[512];
        mirror_path(full, mirror, sizeof(mirror));
        sd_card_remove(mirror);
    }

    return ESP_OK;
}

esp_err_t config_part_rename(const char *src, const char *dst)
{
    if (!src || !dst) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    char fsrc[512], fdst[512];
    if (strncmp(src, CONFIG_MOUNT_POINT, strlen(CONFIG_MOUNT_POINT)) == 0) {
        strlcpy(fsrc, src, sizeof(fsrc));
    } else if (src[0] == '/') {
        snprintf(fsrc, sizeof(fsrc), "%s%s", CONFIG_MOUNT_POINT, src);
    } else {
        snprintf(fsrc, sizeof(fsrc), "%s/%s", CONFIG_MOUNT_POINT, src);
    }
    if (strncmp(dst, CONFIG_MOUNT_POINT, strlen(CONFIG_MOUNT_POINT)) == 0) {
        strlcpy(fdst, dst, sizeof(fdst));
    } else if (dst[0] == '/') {
        snprintf(fdst, sizeof(fdst), "%s%s", CONFIG_MOUNT_POINT, dst);
    } else {
        snprintf(fdst, sizeof(fdst), "%s/%s", CONFIG_MOUNT_POINT, dst);
    }

    if (rename(fsrc, fdst) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed: errno %d", fsrc, fdst, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool config_part_exists(const char *path)
{
    if (!s_mounted || !path) return false;
    char full[512];
    snprintf(full, sizeof(full), "%s%s", CONFIG_MOUNT_POINT, path[0] == '/' ? path : "/");
    struct stat st;
    return (stat(full, &st) == 0);
}

/* Recursively mirror a directory from config to SD */
static void mirror_dir(const char *lfs_base, const char *sd_base)
{
    DIR *d = opendir(lfs_base);
    if (!d) return;

    sd_card_mkdir(sd_base);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char lfs_path[512], sd_path[512];
        snprintf(lfs_path, sizeof(lfs_path), "%s/%s", lfs_base, ent->d_name);
        snprintf(sd_path,  sizeof(sd_path),  "%s/%s", sd_base,  ent->d_name);

        struct stat st;
        if (stat(lfs_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            mirror_dir(lfs_path, sd_path);
        } else {
            /* Read file into a temporary heap buffer (files are small — profiles ≤8KB) */
            size_t sz = (size_t)st.st_size;
            void *buf = malloc(sz);
            if (!buf) {
                ESP_LOGW(TAG, "mirror alloc failed for %s", lfs_path);
                continue;
            }
            FILE *f = fopen(lfs_path, "rb");
            if (f) {
                size_t got = fread(buf, 1, sz, f);
                fclose(f);
                if (got == sz) {
                    sd_card_write_file(sd_path, buf, sz);
                }
            }
            free(buf);
        }
    }
    closedir(d);
}

void config_part_mirror_to_sd(void)
{
    if (!s_mounted || sd_card_get_state() != SD_STATE_MOUNTED) return;
    ESP_LOGI(TAG, "Mirroring config -> SD %s", SD_MIRROR_ROOT);
    mirror_dir(CONFIG_MOUNT_POINT, SD_MIRROR_ROOT);
}

esp_err_t config_part_restore_from_sd(void)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (sd_card_get_state() != SD_STATE_MOUNTED) {
        ESP_LOGW(TAG, "SD not mounted, cannot restore config");
        return ESP_ERR_NOT_FOUND;
    }

    /* Walk SD mirror and copy files back to LittleFS */
    DIR *d = opendir(SD_MIRROR_ROOT);
    if (!d) {
        ESP_LOGI(TAG, "No SD mirror at %s, nothing to restore", SD_MIRROR_ROOT);
        return ESP_OK;
    }
    closedir(d);

    /* Use a simple internal-RAM staging buffer — SD DMA cannot go to PSRAM */
    static uint8_t s_restore_buf[16 * 1024];

    /* Reuse mirror_dir logic in reverse: read from SD, write to LittleFS.
     * For simplicity we use the SD read + config_part_write path. */
    struct dirent *ent;
    d = opendir(SD_MIRROR_ROOT);
    if (!d) return ESP_ERR_NOT_FOUND;

    esp_err_t result = ESP_OK;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char sd_path[512], lfs_rel[512];
        snprintf(sd_path,  sizeof(sd_path),  "%s/%s", SD_MIRROR_ROOT, ent->d_name);
        snprintf(lfs_rel,  sizeof(lfs_rel),  "/%s",   ent->d_name);

        struct stat st;
        if (stat(sd_path, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        size_t got = 0;
        esp_err_t err = sd_card_read_file(sd_path, s_restore_buf,
                                          sizeof(s_restore_buf), &got);
        if (err != ESP_OK || got == 0) {
            ESP_LOGW(TAG, "Restore: failed to read %s", sd_path);
            result = err;
            continue;
        }
        err = config_part_write(lfs_rel, s_restore_buf, got);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Restore: failed to write %s", lfs_rel);
            result = err;
        }
    }
    closedir(d);

    ESP_LOGI(TAG, "config restore from SD complete");
    return result;
}
