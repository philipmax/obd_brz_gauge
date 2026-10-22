// storage_util.c — helpers shared by the storage jobs: job lock, theme key /
// manifest parsing, legacy bootmedia length walk, mmap-window flash reads
// and flash <-> SD copies through the shared 16 KB bounce buffer.
#include "app_obd_dsp/storage_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "spi_flash_mmap.h"

#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/theme_mount.h"
#include "media/delta_varint.h"

static const char *TAG = "storage_util";

#define MAP_PAGE     0x10000u            /* MMU page */
#define MAP_WINDOW   (1024u * 1024u)     /* one mmap window */
#define CB_CHUNK     (32u * 1024u)       /* callback granularity inside a window */
#define BM_MANIFEST  0x1000u             /* legacy bootmedia manifest slot */
#define PATH_MAX_LEN 272

/* ------------------------------------------------------------------ */
/* Job lock                                                            */
/* ------------------------------------------------------------------ */

static SemaphoreHandle_t s_job;
static portMUX_TYPE      s_job_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t job_mutex(void)
{
    if (!s_job) {
        SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
        taskENTER_CRITICAL(&s_job_mux);
        if (!s_job) {
            s_job = m;
            m = NULL;
        }
        taskEXIT_CRITICAL(&s_job_mux);
        if (m) vSemaphoreDelete(m);
    }
    return s_job;
}

bool storage_job_lock(TickType_t wait)
{
    SemaphoreHandle_t m = job_mutex();
    return m && xSemaphoreTakeRecursive(m, wait) == pdTRUE;
}

void storage_job_unlock(void)
{
    if (s_job) xSemaphoreGiveRecursive(s_job);
}

bool storage_job_busy(void)
{
    return s_job && xSemaphoreGetMutexHolder(s_job) != NULL;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

void *storage_psram_alloc(size_t len)
{
    void *p = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(len ? len : 1, MALLOC_CAP_8BIT);
    return p;
}

bool storage_has_suffix_ci(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

void storage_sd_mkdir_parents(const char *path)
{
    char tmp[PATH_MAX_LEN];
    if (!path || strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp)) return;
    size_t mp = strlen(SD_MOUNT_POINT);
    if (strncmp(tmp, SD_MOUNT_POINT, mp) != 0 || tmp[mp] != '/') return;
    for (char *p = tmp + mp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            sd_card_mkdir(tmp);
            *p = '/';
        }
    }
}

int64_t storage_sd_file_size(const char *path)
{
    struct stat st;
    if (!path || !sd_card_is_mounted() || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

/* ------------------------------------------------------------------ */
/* Theme key                                                           */
/* ------------------------------------------------------------------ */

static bool is_dos_device(const char *s)
{
    static const char *const names[] = { "CON", "PRN", "AUX", "NUL" };
    size_t n = strcspn(s, ".");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (n == 3 && strncasecmp(s, names[i], 3) == 0) return true;
    }
    return n == 4 && (strncasecmp(s, "COM", 3) == 0 || strncasecmp(s, "LPT", 3) == 0) &&
           s[3] >= '1' && s[3] <= '9';
}

void storage_theme_key(const char *theme_id, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    size_t max = out_len < STORAGE_THEME_KEY_LEN ? out_len : STORAGE_THEME_KEY_LEN;
    size_t n = 0;
    if (theme_id) {
        while (*theme_id == '.' || *theme_id == ' ') theme_id++;
        for (; *theme_id && n + 1 < max; theme_id++) {
            char c = *theme_id;
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
            out[n++] = ok ? c : '_';
        }
    }
    while (n && (out[n - 1] == '.' || out[n - 1] == ' ')) n--;
    out[n] = '\0';
    if (n == 0) {
        strlcpy(out, "theme", max);
        return;
    }
    if (is_dos_device(out)) {
        if (n + 1 >= max) n = max - 2;
        memmove(out + 1, out, n);
        out[0] = '_';
        out[n + 1] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* theme.bin manifest scanner (no JSON tree, no heap)                  */
/* ------------------------------------------------------------------ */

#define MS_MAX_DEPTH 16

enum { K_OTHER = 0, K_SCHEMA, K_THEME, K_ASSETS, K_PAGES, K_ID, K_NAME, K_VERSION, K_AUTHOR,
       K_OFFSET, K_SIZE, K_LOFF, K_LSIZE };
enum { TOP_NONE = 0, TOP_THEME, TOP_ASSETS, TOP_PAGES };
enum { H_OFF = 1, H_SIZE = 2, H_LOFF = 4, H_LSIZE = 8 };

typedef struct {
    uint32_t off, size, loff, lsize;
    uint8_t  have;
} ms_acc_t;

typedef struct {
    const char *p;
    const char *end;
    uint32_t    cap;
    uint32_t    used;
    bool        bad_range;
    char        schema[8];
    storage_theme_meta_t *out;
} ms_t;

static void ms_ws(ms_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}

/* Append one code point / raw sequence only if it fits whole. */
static void ms_put(char *dst, size_t cap, size_t *n, bool *full, const char *seq, size_t len)
{
    if (!dst || *full) return;
    if (*n + len >= cap) {
        *full = true;
        return;
    }
    memcpy(dst + *n, seq, len);
    *n += len;
}

static int ms_hex4(const char *p, const char *end, uint32_t *out)
{
    if (end - p < 4) return -1;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static size_t utf8_encode(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* *s->p == '"'. dst may be NULL (skip). Truncates on a character boundary. */
static bool ms_string(ms_t *s, char *dst, size_t cap)
{
    size_t n = 0;
    bool full = false;
    s->p++;
    while (s->p < s->end) {
        unsigned char c = (unsigned char)*s->p;
        if (c == '"') {
            s->p++;
            if (dst && cap) dst[n] = '\0';
            return true;
        }
        if (c < 0x20) return false;
        if (c == '\\') {
            if (s->end - s->p < 2) return false;
            char e = s->p[1];
            char o[4];
            size_t ol = 1;
            s->p += 2;
            switch (e) {
            case '"': case '\\': case '/': o[0] = e; break;
            case 'b': o[0] = '\b'; break;
            case 'f': o[0] = '\f'; break;
            case 'n': o[0] = '\n'; break;
            case 'r': o[0] = '\r'; break;
            case 't': o[0] = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (ms_hex4(s->p, s->end, &cp) != 0) return false;
                s->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u' &&
                        ms_hex4(s->p + 2, s->end, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        s->p += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = '?';
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = '?';
                }
                if (cp == 0) cp = '?';
                ol = utf8_encode(cp, o);
                break;
            }
            default:
                return false;
            }
            ms_put(dst, cap, &n, &full, o, ol);
            continue;
        }
        /* raw byte or UTF-8 sequence, copied whole */
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if ((size_t)(s->end - s->p) < len) len = (size_t)(s->end - s->p);
        for (size_t i = 1; i < len; i++) {
            if (((unsigned char)s->p[i] & 0xC0) != 0x80) {
                len = i;
                break;
            }
        }
        ms_put(dst, cap, &n, &full, s->p, len);
        s->p += len;
    }
    return false;
}

static bool ms_number(ms_t *s, uint32_t *val, bool *is_u32)
{
    bool neg = false, frac = false;
    uint64_t v = 0;
    if (*s->p == '-') {
        neg = true;
        s->p++;
    }
    const char *start = s->p;
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') {
        if (v <= 0xFFFFFFFFull) v = v * 10 + (uint64_t)(*s->p - '0');
        s->p++;
    }
    if (s->p == start) return false;
    if (s->p < s->end && *s->p == '.') {
        frac = true;
        s->p++;
        while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
    }
    if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
        frac = true;
        s->p++;
        if (s->p < s->end && (*s->p == '+' || *s->p == '-')) s->p++;
        while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
    }
    *is_u32 = !neg && !frac && v <= 0xFFFFFFFFull;
    *val = (uint32_t)v;
    return true;
}

static bool ms_literal(ms_t *s)
{
    static const char *const lits[] = { "true", "false", "null" };
    for (size_t i = 0; i < 3; i++) {
        size_t l = strlen(lits[i]);
        if ((size_t)(s->end - s->p) >= l && memcmp(s->p, lits[i], l) == 0) {
            s->p += l;
            return true;
        }
    }
    return false;
}

static int ms_classify(const char *k)
{
    static const struct { const char *name; int id; } keys[] = {
        { "schema_version", K_SCHEMA }, { "theme", K_THEME }, { "assets", K_ASSETS },
        { "pages", K_PAGES }, { "id", K_ID }, { "name", K_NAME }, { "version", K_VERSION },
        { "author", K_AUTHOR }, { "offset", K_OFFSET }, { "size", K_SIZE },
        { "layout_data_offset", K_LOFF }, { "layout_data_size", K_LSIZE },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(k, keys[i].name) == 0) return keys[i].id;
    }
    return K_OTHER;
}

static void ms_range(ms_t *s, uint32_t off, uint32_t size)
{
    if (size == 0) return;
    uint64_t end = (uint64_t)off + size;
    if (off < STORAGE_THEME_MANIFEST_SIZE || end > s->cap) {
        s->bad_range = true;
        return;
    }
    if (end > s->used) s->used = (uint32_t)end;
}

static bool ms_value(ms_t *s, int depth, int top, int key, ms_acc_t *acc);

static bool ms_object(ms_t *s, int depth, int top)
{
    ms_acc_t acc = {0};
    s->p++;   /* '{' */
    ms_ws(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        return true;
    }
    while (s->p < s->end) {
        char kbuf[24];
        if (*s->p != '"' || !ms_string(s, kbuf, sizeof(kbuf))) return false;
        int k = ms_classify(kbuf);
        ms_ws(s);
        if (s->p >= s->end || *s->p != ':') return false;
        s->p++;
        ms_ws(s);
        int child_top = top;
        if (depth == 1) {
            child_top = (k == K_THEME) ? TOP_THEME : (k == K_ASSETS) ? TOP_ASSETS :
                        (k == K_PAGES) ? TOP_PAGES : TOP_NONE;
        }
        if (!ms_value(s, depth, child_top, k, &acc)) return false;
        ms_ws(s);
        if (s->p >= s->end) return false;
        if (*s->p == ',') {
            s->p++;
            ms_ws(s);
            continue;
        }
        if (*s->p != '}') return false;
        s->p++;
        if (top == TOP_ASSETS && (acc.have & (H_OFF | H_SIZE)) == (H_OFF | H_SIZE)) {
            ms_range(s, acc.off, acc.size);
        }
        if (top == TOP_PAGES && (acc.have & (H_LOFF | H_LSIZE)) == (H_LOFF | H_LSIZE)) {
            ms_range(s, acc.loff, acc.lsize);
        }
        return true;
    }
    return false;
}

static bool ms_array(ms_t *s, int depth, int top)
{
    s->p++;   /* '[' */
    ms_ws(s);
    if (s->p < s->end && *s->p == ']') {
        s->p++;
        return true;
    }
    while (s->p < s->end) {
        if (!ms_value(s, depth, top, K_OTHER, NULL)) return false;
        ms_ws(s);
        if (s->p >= s->end) return false;
        if (*s->p == ',') {
            s->p++;
            ms_ws(s);
            continue;
        }
        if (*s->p != ']') return false;
        s->p++;
        return true;
    }
    return false;
}

/* depth = depth of the container holding this value (root object = 1). */
static bool ms_value(ms_t *s, int depth, int top, int key, ms_acc_t *acc)
{
    if (s->p >= s->end) return false;
    char c = *s->p;
    if (c == '{' || c == '[') {
        if (depth + 1 > MS_MAX_DEPTH) return false;
        return c == '{' ? ms_object(s, depth + 1, top) : ms_array(s, depth + 1, top);
    }
    if (c == '"') {
        char *dst = NULL;
        size_t cap = 0;
        if (depth == 1 && key == K_SCHEMA) {
            dst = s->schema;
            cap = sizeof(s->schema);
        } else if (depth == 2 && top == TOP_THEME) {
            switch (key) {
            case K_ID:      dst = s->out->id;      cap = sizeof(s->out->id);      break;
            case K_NAME:    dst = s->out->name;    cap = sizeof(s->out->name);    break;
            case K_VERSION: dst = s->out->version; cap = sizeof(s->out->version); break;
            case K_AUTHOR:  dst = s->out->author;  cap = sizeof(s->out->author);  break;
            default: break;
            }
        }
        return ms_string(s, dst, cap);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        uint32_t v = 0;
        bool ok = false;
        if (!ms_number(s, &v, &ok)) return false;
        if (acc && ok) {
            switch (key) {
            case K_OFFSET: acc->off = v;   acc->have |= H_OFF;   break;
            case K_SIZE:   acc->size = v;  acc->have |= H_SIZE;  break;
            case K_LOFF:   acc->loff = v;  acc->have |= H_LOFF;  break;
            case K_LSIZE:  acc->lsize = v; acc->have |= H_LSIZE; break;
            default: break;
            }
        } else if (acc && (key == K_OFFSET || key == K_SIZE || key == K_LOFF || key == K_LSIZE) &&
                   (top == TOP_ASSETS || top == TOP_PAGES)) {
            s->bad_range = true;   /* negative / fractional / huge offset */
        }
        return true;
    }
    return ms_literal(s);
}

esp_err_t storage_theme_manifest_parse(const uint8_t *buf, size_t len, uint32_t blob_cap,
                                       storage_theme_meta_t *out)
{
    if (!buf || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (len == 0 || buf[0] == 0xFF || buf[0] == 0x00) return ESP_ERR_NOT_FOUND;
    if (len > STORAGE_THEME_MANIFEST_SIZE) len = STORAGE_THEME_MANIFEST_SIZE;
    size_t n = 0;
    while (n < len && buf[n] != 0xFF && buf[n] != 0x00) n++;

    ms_t s = {
        .p = (const char *)buf, .end = (const char *)buf + n,
        .cap = blob_cap, .used = STORAGE_THEME_MANIFEST_SIZE, .out = out,
    };
    ms_ws(&s);
    if (s.p >= s.end || *s.p != '{' || !ms_object(&s, 1, TOP_NONE)) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(s.schema, "1.0") != 0) return ESP_ERR_NOT_SUPPORTED;
    if (!out->id[0]) return ESP_ERR_INVALID_RESPONSE;
    if (s.bad_range || blob_cap < STORAGE_THEME_MANIFEST_SIZE) return ESP_ERR_INVALID_SIZE;
    out->used_len = s.used;
    return ESP_OK;
}

esp_err_t storage_theme_meta_from_flash(storage_theme_meta_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    size_t cap = theme_get_partition_size();
    if (cap < STORAGE_THEME_MANIFEST_SIZE) return ESP_ERR_NOT_FOUND;
    uint8_t *buf = storage_psram_alloc(STORAGE_THEME_MANIFEST_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t err = theme_raw_read(0, buf, STORAGE_THEME_MANIFEST_SIZE);
    if (err == ESP_OK) err = storage_theme_manifest_parse(buf, STORAGE_THEME_MANIFEST_SIZE, (uint32_t)cap, out);
    heap_caps_free(buf);
    return err;
}

esp_err_t storage_theme_meta_from_file(const char *path, storage_theme_meta_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    int64_t size = storage_sd_file_size(path);
    if (size < 0) return ESP_ERR_NOT_FOUND;
    if (size > 0xFFFFFFFFll) return ESP_ERR_INVALID_SIZE;
    size_t want = size < STORAGE_THEME_MANIFEST_SIZE ? (size_t)size : STORAGE_THEME_MANIFEST_SIZE;
    uint8_t *buf = storage_psram_alloc(STORAGE_THEME_MANIFEST_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_FAIL;
    FILE *f = fopen(path, "rb");
    if (f) {
        setvbuf(f, NULL, _IONBF, 0);
        size_t got = want ? sd_card_read(f, buf, want) : 0;
        fclose(f);
        if (got == want) err = storage_theme_manifest_parse(buf, want, (uint32_t)size, out);
    }
    heap_caps_free(buf);
    return err;
}

/* ------------------------------------------------------------------ */
/* Flash reads                                                         */
/* ------------------------------------------------------------------ */

esp_err_t storage_flash_foreach(uint32_t abs_addr, uint32_t len, storage_chunk_cb_t cb, void *ctx)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    uint8_t *fallback = NULL;
    esp_err_t err = ESP_OK;
    bool stop = false;

    while (len && !stop) {
        uint32_t base = abs_addr & ~(MAP_PAGE - 1);
        uint32_t skip = abs_addr - base;
        uint32_t n = MAP_WINDOW - skip;
        if (n > len) n = len;
        uint32_t map_len = (skip + n + MAP_PAGE - 1) & ~(MAP_PAGE - 1);

        const void *ptr = NULL;
        spi_flash_mmap_handle_t h = 0;
        if (spi_flash_mmap(base, map_len, SPI_FLASH_MMAP_DATA, &ptr, &h) == ESP_OK) {
            const uint8_t *p = (const uint8_t *)ptr + skip;
            for (uint32_t off = 0; off < n && !stop; off += CB_CHUNK) {
                uint32_t m = n - off < CB_CHUNK ? n - off : CB_CHUNK;
                stop = !cb(p + off, m, ctx);
            }
            spi_flash_munmap(h);
        } else {
            if (!fallback) {
                fallback = storage_psram_alloc(SD_IO_BUF_SIZE);
                if (!fallback) return ESP_ERR_NO_MEM;
            }
            for (uint32_t off = 0; off < n && !stop; off += SD_IO_BUF_SIZE) {
                uint32_t m = n - off < SD_IO_BUF_SIZE ? n - off : SD_IO_BUF_SIZE;
                err = esp_flash_read(NULL, fallback, abs_addr + off, m);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "flash read 0x%lx: %s", (unsigned long)(abs_addr + off), esp_err_to_name(err));
                    stop = true;
                    break;
                }
                stop = !cb(fallback, m, ctx);
            }
        }
        abs_addr += n;
        len -= n;
        taskYIELD();
    }
    if (fallback) heap_caps_free(fallback);
    return err;
}

static bool crc_cb(const uint8_t *data, size_t len, void *ctx)
{
    uint32_t *crc = (uint32_t *)ctx;
    *crc = esp_rom_crc32_le(*crc, data, (uint32_t)len);
    return true;
}

esp_err_t storage_flash_crc32(uint32_t abs_addr, uint32_t len, uint32_t *crc)
{
    if (!crc) return ESP_ERR_INVALID_ARG;
    uint32_t c = 0;
    esp_err_t err = storage_flash_foreach(abs_addr, len, crc_cb, &c);
    if (err == ESP_OK) *crc = c;
    return err;
}

/* ------------------------------------------------------------------ */
/* Legacy bootmedia stream walk                                        */
/* ------------------------------------------------------------------ */

enum { BW_COUNT = 0, BW_VARINT, BW_COLOR };

typedef struct {
    uint32_t frames_left;
    uint32_t cells;
    uint32_t changes_left;
    uint32_t prev_idx;
    uint32_t acc;
    uint32_t consumed;
    uint32_t crc;
    uint8_t  st;
    uint8_t  nbytes;
    bool     u32;
    bool     done;
    bool     bad;
} bm_walk_t;

/* One stream byte. Returns false when the walk is finished or broken. */
static inline bool bw_byte(bm_walk_t *w, uint8_t b)
{
    bool entry_done = false;
    switch (w->st) {
    case BW_COUNT:
        w->acc |= (uint32_t)b << (8 * w->nbytes);
        if (++w->nbytes < (w->u32 ? 4 : 2)) return true;
        w->changes_left = w->acc;
        w->acc = 0;
        w->nbytes = 0;
        w->prev_idx = UINT32_MAX;   /* same rules as dv_stream_next_frame() */
        if (w->changes_left) {
            w->st = BW_VARINT;
            return true;
        }
        break;   /* empty frame */
    case BW_VARINT: {
        w->acc |= (uint32_t)(b & 0x7F) << (7 * w->nbytes);
        w->nbytes++;
        if (b & 0x80) {
            if (w->nbytes >= 4) {
                w->bad = true;
                return false;
            }
            return true;
        }
        uint32_t delta = w->acc >> 1;
        bool black = (w->acc & 1u) != 0;
        w->acc = 0;
        w->nbytes = 0;
        uint64_t idx = (w->prev_idx == UINT32_MAX) ? delta : (uint64_t)w->prev_idx + delta;
        if (idx >= w->cells) {
            w->bad = true;
            return false;
        }
        w->prev_idx = (uint32_t)idx;
        if (!black) {
            w->st = BW_COLOR;
            return true;
        }
        entry_done = true;
        break;
    }
    case BW_COLOR:
        if (++w->nbytes < 2) return true;
        w->nbytes = 0;
        entry_done = true;
        break;
    default:
        w->bad = true;
        return false;
    }
    if (entry_done && --w->changes_left) {
        w->st = BW_VARINT;
        return true;
    }
    /* frame complete */
    w->st = BW_COUNT;
    if (--w->frames_left == 0) {
        w->done = true;
        return false;
    }
    return true;
}

static bool bw_cb(const uint8_t *data, size_t len, void *ctx)
{
    bm_walk_t *w = (bm_walk_t *)ctx;
    size_t i = 0;
    bool more = true;
    while (i < len && more) {
        more = bw_byte(w, data[i]);
        i++;
    }
    w->crc = esp_rom_crc32_le(w->crc, data, (uint32_t)i);
    w->consumed += (uint32_t)i;
    return more;
}

esp_err_t storage_bootmedia_probe(uint32_t abs_addr, uint32_t avail, uint32_t *image_len, uint32_t *crc)
{
    if (avail <= BM_MANIFEST) return ESP_ERR_INVALID_SIZE;
    char *man = storage_psram_alloc(BM_MANIFEST);
    if (!man) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_flash_read(NULL, man, abs_addr, BM_MANIFEST);
    if (err != ESP_OK) {
        heap_caps_free(man);
        return err;
    }
    if (man[0] < 'a' || man[0] > 'z') {
        heap_caps_free(man);
        return ESP_ERR_NOT_FOUND;
    }
    dv_manifest_t m;
    if (dv_parse_manifest(man, BM_MANIFEST, &m) != DV_OK) {
        heap_caps_free(man);
        ESP_LOGW(TAG, "bootmedia @0x%lx: bad manifest", (unsigned long)abs_addr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t limit = avail - BM_MANIFEST;
    bm_walk_t w = {
        .frames_left = m.frame_count,
        .cells = (uint32_t)m.grid_w * m.grid_h,
        .u32 = m.count_u32,
        .crc = esp_rom_crc32_le(0, (const uint8_t *)man, BM_MANIFEST),
    };
    heap_caps_free(man);

    err = storage_flash_foreach(abs_addr + BM_MANIFEST, limit, bw_cb, &w);
    if (err != ESP_OK) return err;
    if (!w.done) {
        ESP_LOGW(TAG, "bootmedia @0x%lx: stream %s after %lu bytes (%lu frames left)",
                 (unsigned long)abs_addr, w.bad ? "corrupt" : "truncated",
                 (unsigned long)w.consumed, (unsigned long)w.frames_left);
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* Keep a declared binary_size if it is longer (trailing padding the
     * player may still expect to be readable). */
    uint32_t stream_len = w.consumed;
    if (m.binary_size > stream_len && m.binary_size <= limit) {
        uint32_t tail = m.binary_size - stream_len;
        err = storage_flash_foreach(abs_addr + BM_MANIFEST + stream_len, tail, crc_cb, &w.crc);
        if (err != ESP_OK) return err;
        stream_len = m.binary_size;
    }
    if (image_len) *image_len = BM_MANIFEST + stream_len;
    if (crc) *crc = w.crc;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Flash -> SD copy                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE    *f;
    uint32_t crc;
    uint32_t done;
    bool     fail;
} cp_ctx_t;

static bool cp_cb(const uint8_t *data, size_t len, void *ctx)
{
    cp_ctx_t *c = (cp_ctx_t *)ctx;
    while (len) {
        if (!sd_card_is_mounted()) {
            c->fail = true;
            return false;
        }
        size_t m = len < SD_IO_BUF_SIZE ? len : SD_IO_BUF_SIZE;
        size_t put;
        uint8_t *bb = sd_card_io_buf_take(portMAX_DELAY);
        if (bb) {
            memcpy(bb, data, m);
            put = fwrite(bb, 1, m, c->f);
            sd_card_io_buf_give();
        } else {
            put = fwrite(data, 1, m, c->f);
        }
        if (put != m) {
            c->fail = true;
            return false;
        }
        c->crc = esp_rom_crc32_le(c->crc, data, (uint32_t)m);
        c->done += (uint32_t)m;
        data += m;
        len -= m;
    }
    return true;
}

esp_err_t storage_copy_flash_to_sd(uint32_t abs_addr, uint32_t len, const char *dst, uint32_t *crc)
{
    if (!dst) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    char part[PATH_MAX_LEN];
    if (snprintf(part, sizeof(part), "%s.part", dst) >= (int)sizeof(part)) return ESP_ERR_INVALID_ARG;
    storage_sd_mkdir_parents(dst);

    FILE *f = fopen(part, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open %s: errno %d", part, errno);
        return ESP_FAIL;
    }
    setvbuf(f, NULL, _IONBF, 0);
    cp_ctx_t c = { .f = f };
    esp_err_t err = storage_flash_foreach(abs_addr, len, cp_cb, &c);
    bool ok = (err == ESP_OK) && !c.fail && c.done == len &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        ESP_LOGW(TAG, "copy to %s failed (%lu/%lu)", dst, (unsigned long)c.done, (unsigned long)len);
        unlink(part);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    err = sd_card_rename(part, dst);
    if (err != ESP_OK) {
        unlink(part);
        return err;
    }
    if (crc) *crc = c.crc;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* SD reads                                                            */
/* ------------------------------------------------------------------ */

esp_err_t storage_sd_file_foreach(const char *path, uint32_t off, uint32_t len,
                                  storage_chunk_cb_t cb, void *ctx)
{
    if (!path || !cb) return ESP_ERR_INVALID_ARG;
    int64_t size = storage_sd_file_size(path);
    if (size < 0) return ESP_ERR_NOT_FOUND;
    if ((int64_t)off > size) return ESP_ERR_INVALID_SIZE;
    if (len == 0) len = (uint32_t)(size - off);
    if ((int64_t)off + len > size) return ESP_ERR_INVALID_SIZE;

    const int64_t started = esp_timer_get_time();
    int64_t wait_us = 0, read_us = 0, callback_us = 0;
    uint32_t read_bytes = 0;
    /* Newlib's unbuffered fread refills its one-byte FILE buffer on this
     * target. Use the VFS bulk path so FatFS receives whole DMA-sized reads. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ESP_FAIL;
    if (off && lseek(fd, (off_t)off, SEEK_SET) < 0) {
        close(fd);
        return ESP_FAIL;
    }
    uint8_t *own = NULL;
    esp_err_t err = ESP_OK;
    while (len) {
        size_t m = len < SD_IO_BUF_SIZE ? len : SD_IO_BUF_SIZE;
        int64_t phase = esp_timer_get_time();
        uint8_t *bb = sd_card_io_buf_take(portMAX_DELAY);
        wait_us += esp_timer_get_time() - phase;
        uint8_t *buf = bb;
        if (!buf) {
            if (!own) own = heap_caps_malloc(SD_IO_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (!own) own = storage_psram_alloc(SD_IO_BUF_SIZE);
            buf = own;
        }
        if (!buf) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        phase = esp_timer_get_time();
        size_t got = 0;
        while (got < m) {
            ssize_t n = read(fd, buf + got, m - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t)n;
        }
        read_us += esp_timer_get_time() - phase;
        read_bytes += got;
        phase = esp_timer_get_time();
        bool more = (got == m) && cb(buf, m, ctx);
        callback_us += esp_timer_get_time() - phase;
        if (bb) sd_card_io_buf_give();
        if (got != m) {
            err = ESP_FAIL;
            break;
        }
        len -= (uint32_t)m;
        if (!more) break;
    }
    close(fd);
    if (own) heap_caps_free(own);
    const int64_t elapsed = esp_timer_get_time() - started;
    if (elapsed >= 100000) {
        ESP_LOGI(TAG, "SD stream %s: %lu bytes, total %lld ms, buffer wait %lld ms, read %lld ms, callback %lld ms",
                 path, (unsigned long)read_bytes, (long long)(elapsed / 1000),
                 (long long)(wait_us / 1000), (long long)(read_us / 1000),
                 (long long)(callback_us / 1000));
    }
    return err;
}

