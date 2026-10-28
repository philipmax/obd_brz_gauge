// upload_sink.c — shared streaming upload path (see upload_sink.h)
#include "upload_sink.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/base64.h"

#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/boot_media_mount.h" /* legacy SD manifest padding */
#include "bsp_obd_dsp/config_part.h"
#include "bsp_obd_dsp/sd_card.h"

static const char *TAG = "upload";

#define SD_SYNC_STEP        (1024 * 1024)   // fsync SD uploads every 1 MB
#define OTA_STAGE_MAX       (16 * 1024)     // internal staging for esp_ota_write
#define OTA_STAGE_MIN       4096
#define RECV_MAX_TIMEOUTS   3               // consecutive recv timeouts before giving up
#define B64_DECODE_CAP      (UPLOAD_RECV_CHUNK / 4 * 3 + 3)

/* ------------------------------------------------------------------ */
/* SHA-256                                                             */
/* ------------------------------------------------------------------ */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool upload_sha_from_hex(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint8_t *alloc_payload(size_t size, const char *label)
{
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    size_t free_bytes = heap_caps_get_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    ESP_LOGI(TAG, "%s buffer: need=%u, PSRAM free=%u, largest=%u",
             label, (unsigned)size, (unsigned)free_bytes, (unsigned)largest);
    if (largest < size) {
        return NULL;
    }
    return heap_caps_malloc(size, caps);
}

/* Per-request staging buffer: PSRAM, internal RAM as a fallback. */
static void *alloc_stage(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void part_path(const upload_sink_t *s, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s.part", s->path);
}

static bool pad_enabled(const upload_sink_t *s)
{
    return s->pad_to > s->pad_at;
}

/* Physical file offset of logical payload offset off. */
static uint32_t file_offset_of(const upload_sink_t *s, uint32_t off)
{
    return (pad_enabled(s) && off > s->pad_at) ? off + (s->pad_to - s->pad_at) : off;
}

static esp_err_t file_put(upload_sink_t *s, const void *src, size_t len)
{
    if (sd_card_write(s->fp, src, len) != len) {
        ESP_LOGE(TAG, "SD write failed at %u: errno %d", (unsigned)s->file_pos, errno);
        s->err = "sd write failed";
        return ESP_FAIL;
    }
    s->file_pos += len;
    if (s->file_pos - s->synced >= SD_SYNC_STEP) {
        if (fflush(s->fp) != 0 || fsync(fileno(s->fp)) != 0) {
            s->err = "sd sync failed";
            return ESP_FAIL;
        }
        s->synced = s->file_pos;
    }
    return ESP_OK;
}

static esp_err_t file_write(upload_sink_t *s, const uint8_t *data, size_t len)
{
    size_t done = 0;
    if (pad_enabled(s) && s->received < s->pad_at) {
        size_t n = s->pad_at - s->received;
        if (n > len) n = len;
        esp_err_t err = file_put(s, data, n);
        if (err != ESP_OK) return err;
        done = n;
    }
    if (done == len) return ESP_OK;

    if (pad_enabled(s) && s->file_pos < s->pad_to) {
        uint8_t ff[256];
        memset(ff, 0xFF, sizeof(ff));
        while (s->file_pos < s->pad_to) {
            size_t n = s->pad_to - s->file_pos;
            if (n > sizeof(ff)) n = sizeof(ff);
            esp_err_t err = file_put(s, ff, n);
            if (err != ESP_OK) return err;
        }
    }
    return file_put(s, data + done, len - done);
}

/* Undo a failed request: back to logical offset off with the saved digest. */
static void rollback(upload_sink_t *s, uint32_t off, const mbedtls_sha256_context *snap)
{
    s->received = off;
    if (s->sha_running) {
        mbedtls_sha256_free(&s->sha);
        mbedtls_sha256_init(&s->sha);
        mbedtls_sha256_clone(&s->sha, snap);
    }
    if (s->kind == UPLOAD_SINK_SD_FILE && s->fp) {
        uint32_t pos = file_offset_of(s, off);
        if (fseek(s->fp, (long)pos, SEEK_SET) == 0) {
            s->file_pos = pos;
            if (s->synced > pos) s->synced = pos;
        } else {
            s->failed = true;
            s->err = "sd rollback failed";
        }
    }
}

/* Compact base64 text in place, dropping whitespace. Returns the new length. */
static size_t strip_ws(char *buf, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
            buf[o++] = c;
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t upload_sink_open(upload_sink_t *s, upload_sink_kind_t kind, uint32_t expected,
                           const uint8_t *sha, const char *path, uint32_t manifest_size)
{
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->expected = expected;
    s->manifest_size = manifest_size;
    if (sha) {
        memcpy(s->sha_expected, sha, sizeof(s->sha_expected));
        s->has_sha = true;
    }

    esp_err_t err = ESP_OK;
    switch (kind) {
    case UPLOAD_SINK_FIRMWARE:
        s->ota_part = esp_ota_get_next_update_partition(NULL);
        if (!s->ota_part) {
            s->err = "no ota partition";
            err = ESP_ERR_NOT_FOUND;
        } else if (expected == 0 || expected > s->ota_part->size) {
            s->err = "too large";
            err = ESP_ERR_INVALID_SIZE;
        } else if (!(s->buf = alloc_payload(expected, "firmware"))) {
            s->err = "no memory";
            err = ESP_ERR_NO_MEM;
        }
        break;

    case UPLOAD_SINK_THEME:
    case UPLOAD_SINK_BOOTMEDIA:
        s->err = "media requires SD_FILE";
        err = ESP_ERR_NOT_SUPPORTED;
        break;

    case UPLOAD_SINK_SD_FILE: {
        char part[UPLOAD_PATH_MAX + 8];
        if (!path || strlen(path) >= UPLOAD_PATH_MAX ||
            strncmp(path, SD_MOUNT_POINT "/", strlen(SD_MOUNT_POINT) + 1) != 0) {
            s->err = "bad path";
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        if (!sd_card_is_mounted()) {
            s->err = "sd not mounted";
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (manifest_size > 0) {
            if (manifest_size > BOOT_MEDIA_BIN_OFFSET) {
                s->err = "invalid size";
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            s->pad_at = manifest_size;
            s->pad_to = BOOT_MEDIA_BIN_OFFSET;
        }
        strlcpy(s->path, path, sizeof(s->path));
        storage_sd_mkdir_parents(s->path);
        part_path(s, part, sizeof(part));
        s->fp = fopen(part, "wb");
        if (!s->fp) {
            ESP_LOGE(TAG, "open %s failed: errno %d", part, errno);
            s->err = "sd open failed";
            err = ESP_FAIL;
        }
        break;
    }

    case UPLOAD_SINK_CONFIG_FILE:
        if (!path || strlen(path) >= UPLOAD_PATH_MAX ||
            strncmp(path, CONFIG_MOUNT_POINT "/", strlen(CONFIG_MOUNT_POINT) + 1) != 0) {
            s->err = "bad path";
            err = ESP_ERR_INVALID_ARG;
        } else if (!config_part_is_mounted()) {
            s->err = "config not mounted";
            err = ESP_ERR_INVALID_STATE;
        } else if (expected > UPLOAD_CONFIG_MAX || !config_part_has_quota(expected)) {
            s->err = "config full";
            err = ESP_ERR_INVALID_SIZE;
        } else if (!(s->buf = alloc_payload(expected + 1, "config"))) {
            s->err = "no memory";
            err = ESP_ERR_NO_MEM;
        } else {
            strlcpy(s->path, path, sizeof(s->path));
        }
        break;

    case UPLOAD_SINK_MEMORY:
        if (!(s->buf = alloc_payload((size_t)expected + 1, "upload"))) {
            s->err = "no memory";
            err = ESP_ERR_NO_MEM;
        }
        break;

    default:
        s->err = "bad sink";
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    if (err != ESP_OK) {
        const char *why = s->err;
        upload_sink_abort(s);
        s->err = why;
        return err;
    }

    mbedtls_sha256_init(&s->sha);
    s->sha_running = true;
    if (mbedtls_sha256_starts(&s->sha, 0) != 0) {
        upload_sink_abort(s);
        s->err = "sha init failed";
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool upload_sink_is_open(const upload_sink_t *s)
{
    return s->kind != UPLOAD_SINK_NONE;
}

esp_err_t upload_sink_write(upload_sink_t *s, const void *data, size_t len)
{
    if (!upload_sink_is_open(s)) {
        s->err = "no upload";
        return ESP_ERR_INVALID_STATE;
    }
    if (s->failed || s->verified || !s->sha_running) {
        s->err = "upload session invalid";
        return ESP_ERR_INVALID_STATE;
    }
    if (len && !data) return ESP_ERR_INVALID_ARG;
    if (s->received > s->expected || len > s->expected - s->received) {
        ESP_LOGE(TAG, "payload overflow: at=%u len=%u total=%u",
                 (unsigned)s->received, (unsigned)len, (unsigned)s->expected);
        s->err = "payload too large";
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0) return ESP_OK;

    if (s->kind == UPLOAD_SINK_SD_FILE) {
        esp_err_t err = file_write(s, data, len);
        if (err != ESP_OK) { s->failed = true; return err; }
    } else {
        memcpy(s->buf + s->received, data, len);
    }
    if (mbedtls_sha256_update(&s->sha, data, len) != 0) {
        s->failed = true;
        s->err = "sha update failed";
        return ESP_FAIL;
    }
    s->received += len;
    return ESP_OK;
}

esp_err_t upload_sink_recv(upload_sink_t *s, httpd_req_t *req, bool base64)
{
    if (!upload_sink_is_open(s)) {
        s->err = "no upload";
        return ESP_ERR_INVALID_STATE;
    }
    if (s->failed || s->verified || !s->sha_running) {
        s->err = "upload session invalid";
        return ESP_ERR_INVALID_STATE;
    }
    if (req->content_len == 0) return ESP_OK;

    // Memory-backed sinks already own the full PSRAM payload. Receive raw
    // bodies directly into their final location instead of staging then
    // copying every byte through upload_sink_write(). SD and base64 paths
    // still need a scratch buffer for streaming/file writes and decoding.
    const bool direct = !base64 && s->kind != UPLOAD_SINK_SD_FILE;
    if (direct && (!s->buf || s->received > s->expected ||
                   req->content_len > s->expected - s->received)) {
        s->err = "payload too large";
        return ESP_ERR_INVALID_SIZE;
    }
    char *rx = direct ? NULL : alloc_stage(UPLOAD_RECV_CHUNK);
    uint8_t *dec = base64 ? alloc_stage(B64_DECODE_CAP) : NULL;
    if ((!direct && !rx) || (base64 && !dec)) {
        free(rx);
        free(dec);
        s->err = "no memory";
        return ESP_ERR_NO_MEM;
    }

    const uint32_t start = s->received;
    mbedtls_sha256_context snap;
    mbedtls_sha256_init(&snap);
    if (s->sha_running) {
        mbedtls_sha256_clone(&snap, &s->sha);
    }

    esp_err_t err = ESP_OK;
    size_t remaining = req->content_len;
    size_t carry = 0;           // base64 chars held back for the next 4-char group
    unsigned timeouts = 0;
    while (remaining > 0) {
        size_t room = UPLOAD_RECV_CHUNK - carry;
        int want = (int)(remaining < room ? remaining : room);
        char *dst = direct ? (char *)s->buf + s->received : rx + carry;
        int got = httpd_req_recv(req, dst, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= RECV_MAX_TIMEOUTS) {
                ESP_LOGE(TAG, "recv timeout limit at %u (%u left)",
                         (unsigned)s->received, (unsigned)remaining);
                s->err = "receive failed";
                err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        if (got <= 0) {
            ESP_LOGE(TAG, "recv failed (%d) at %u (%u left)",
                     got, (unsigned)s->received, (unsigned)remaining);
            s->err = "receive failed";
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        remaining -= (size_t)got;

        if (direct) {
            if (mbedtls_sha256_update(&s->sha, (const unsigned char *)dst, (size_t)got) != 0) {
                s->failed = true;
                s->err = "sha update failed";
                err = ESP_FAIL;
                break;
            }
            s->received += (uint32_t)got;
            continue;
        }
        if (!base64) {
            err = upload_sink_write(s, rx, (size_t)got);
            if (err != ESP_OK) break;
            continue;
        }

        size_t text = strip_ws(rx, carry + (size_t)got);
        size_t use = remaining > 0 ? (text & ~(size_t)3) : text;
        size_t dec_len = 0;
        if (use > 0 &&
            mbedtls_base64_decode(dec, B64_DECODE_CAP, &dec_len, (const unsigned char *)rx, use) != 0) {
            ESP_LOGE(TAG, "base64 decode failed at %u", (unsigned)s->received);
            s->err = "bad base64";
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = upload_sink_write(s, dec, dec_len);
        if (err != ESP_OK) break;
        carry = text - use;
        memmove(rx, rx + use, carry);
    }

    free(rx);
    free(dec);
    if (err != ESP_OK) {
        rollback(s, start, &snap);
    }
    mbedtls_sha256_free(&snap);
    return err;
}

esp_err_t upload_sink_verify(upload_sink_t *s)
{
    if (!upload_sink_is_open(s)) {
        s->err = "no upload";
        return ESP_ERR_INVALID_STATE;
    }
    if (s->failed) {
        s->err = "upload session invalid";
        return ESP_ERR_INVALID_STATE;
    }
    if (s->verified) return ESP_OK;
    if (!s->sha_running) {
        s->err = "upload session invalid";
        return ESP_ERR_INVALID_STATE;
    }
    if (s->received != s->expected) {
        s->err = "size mismatch";
        return ESP_ERR_INVALID_SIZE;
    }
    if (s->sha_running) {
        uint8_t digest[32];
        int ret = mbedtls_sha256_finish(&s->sha, digest);
        mbedtls_sha256_free(&s->sha);
        s->sha_running = false;
        if (ret != 0 || (s->has_sha && memcmp(digest, s->sha_expected, sizeof(digest)) != 0)) {
            s->failed = true;
            s->err = "sha mismatch";
            return ESP_ERR_INVALID_CRC;
        }
    }
    if (s->buf && (s->kind == UPLOAD_SINK_MEMORY || s->kind == UPLOAD_SINK_CONFIG_FILE)) {
        s->buf[s->received] = '\0';
    }
    s->verified = true;
    return ESP_OK;
}

static esp_err_t commit_firmware(upload_sink_t *s)
{
    size_t cap = OTA_STAGE_MAX;
    uint8_t *stage = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stage) {
        cap = OTA_STAGE_MIN;
        stage = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!stage) {
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(s->ota_part, s->expected, &handle);
    for (uint32_t off = 0; err == ESP_OK && off < s->expected; ) {
        size_t n = s->expected - off;
        if (n > cap) n = cap;
        memcpy(stage, s->buf + off, n);   // esp_ota_write must not read PSRAM
        err = esp_ota_write(handle, stage, n);
        off += n;
    }
    if (err == ESP_OK) {
        err = esp_ota_end(handle);
    } else if (handle != 0) {
        esp_ota_abort(handle);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s->ota_part);
    }
    free(stage);
    return err;
}

static esp_err_t commit_sd_file(upload_sink_t *s)
{
    char part[UPLOAD_PATH_MAX + 8];
    part_path(s, part, sizeof(part));

    bool ok = fflush(s->fp) == 0;
    ok = (fsync(fileno(s->fp)) == 0) && ok;
    ok = (fclose(s->fp) == 0) && ok;
    s->fp = NULL;
    if (!ok) {
        s->err = "sd write failed";
        sd_card_remove(part);
        return ESP_FAIL;
    }
    esp_err_t err = sd_card_rename(part, s->path);
    if (err != ESP_OK) {
        s->err = "sd rename failed";
        sd_card_remove(part);
    }
    return err;
}

esp_err_t upload_sink_commit(upload_sink_t *s)
{
    if (!upload_sink_is_open(s) || !s->verified || s->failed || s->received != s->expected) {
        s->err = "not verified";
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    switch (s->kind) {
    case UPLOAD_SINK_FIRMWARE:
        err = commit_firmware(s);
        if (err != ESP_OK) s->err = "flash write failed";
        break;
    case UPLOAD_SINK_THEME:
    case UPLOAD_SINK_BOOTMEDIA:
        err = ESP_ERR_NOT_SUPPORTED;
        s->err = "media requires SD_FILE";
        break;
    case UPLOAD_SINK_SD_FILE:
        err = commit_sd_file(s);
        break;
    case UPLOAD_SINK_CONFIG_FILE:
        err = config_part_write(s->path, s->buf, s->expected);
        if (err != ESP_OK) s->err = "config write failed";
        break;
    case UPLOAD_SINK_MEMORY:
    default:
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit (kind %d, %u B) failed: %s",
                 (int)s->kind, (unsigned)s->expected, esp_err_to_name(err));
    }
    return err;
}

void upload_sink_abort(upload_sink_t *s)
{
    if (s->sha_running) {
        mbedtls_sha256_free(&s->sha);
    }
    if (s->fp) {
        char part[UPLOAD_PATH_MAX + 8];
        fclose(s->fp);
        part_path(s, part, sizeof(part));
        sd_card_remove(part);
    }
    free(s->buf);
    memset(s, 0, sizeof(*s));
}

