/*
 * SD card driver for ESP32-S3 (SDMMC slot 1, 1-bit mode).
 * Hardware (V1 Waveshare 1.85): CLK=GPIO14, CMD=GPIO17, D0=GPIO16,
 * D3 = TCA9554 EXIO3 with a 10K pull-up (high from power-on, so SD mode is
 * selected even before EXIO_Init()).
 *
 * - Mount tries 40 MHz, then 20 MHz. The card is NEVER formatted.
 * - No card-detect pin: while absent/error/ejected, sd_card_poll() probes the
 *   bus every few seconds; while mounted it sends CMD13 and declares the card
 *   removed after two consecutive failures.
 * - One 16 KB internal DMA-capable bounce buffer is allocated at init and
 *   shared through sd_card_io_buf_take()/give() (S3 SDMMC cannot DMA to PSRAM).
 */

#include "sdkconfig.h"
#include "sd_card.h"
#include "app_obd_dsp/perf_stats.h"

#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sd_card";

#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#define SD_PIN_CLK          GPIO_NUM_14
#define SD_PIN_CMD          GPIO_NUM_17
#define SD_PIN_D0           GPIO_NUM_16

#define SD_MAX_FILES        6
#define SD_ALLOC_UNIT       (32 * 1024)
#define SD_FREQ_FAST_KHZ    SDMMC_FREQ_HIGHSPEED   /* 40 MHz */
#define SD_FREQ_SLOW_KHZ    SDMMC_FREQ_DEFAULT     /* 20 MHz */

#define SD_PROBE_ABSENT_MS  3000    /* insertion probe cadence while absent */
#define SD_PROBE_ERROR_MS   10000   /* retry cadence for an unmountable card */
#define SD_REMOVE_FAILS     2       /* consecutive CMD13 failures = removed */
#define SD_PATH_MAX         264

static SemaphoreHandle_t s_lock;        /* guards mount/unmount/state changes */
static SemaphoreHandle_t s_io_mutex;    /* guards s_io_buf */
static StaticSemaphore_t s_lock_storage;
static StaticSemaphore_t s_io_mutex_storage;
static uint8_t          *s_io_buf;      /* SD_IO_BUF_SIZE, DMA | INTERNAL, never freed */
static sdmmc_card_t     *s_card;
static volatile sd_state_t s_state = SD_STATE_ABSENT;
static bool              s_inited;
static uint32_t          s_freq_khz;
static uint32_t          s_pref_khz = SD_FREQ_FAST_KHZ;
static uint8_t           s_fail_cnt;    /* CMD13 failures (mounted) / probe misses (ejected) */
static TickType_t        s_last_probe;

static esp_err_t ensure_directories(void);

/* Tags whose E/W lines would spam the console on every no-card probe. */
static const char *const s_quiet_tags[] = {
    "vfs_fat_sdmmc", "sdmmc_common", "sdmmc_sd", "sdmmc_init", "sdmmc_cmd",
    "sdmmc_req", "sdmmc_periph", "diskio_sdmmc", "vfs_fat",
};
#define QUIET_N (sizeof(s_quiet_tags) / sizeof(s_quiet_tags[0]))

static void quiet_logs(bool quiet, esp_log_level_t saved[QUIET_N])
{
    for (size_t i = 0; i < QUIET_N; i++) {
        if (quiet) {
            saved[i] = esp_log_level_get(s_quiet_tags[i]);
            esp_log_level_set(s_quiet_tags[i], ESP_LOG_NONE);
        } else {
            esp_log_level_set(s_quiet_tags[i], saved[i]);
        }
    }
}

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void slot_config(sdmmc_slot_config_t *slot)
{
    sdmmc_slot_config_t s = SDMMC_SLOT_CONFIG_DEFAULT();
    s.width = 1;
    s.clk = SD_PIN_CLK;
    s.cmd = SD_PIN_CMD;
    s.d0  = SD_PIN_D0;
    s.d1 = s.d2 = s.d3 = s.d4 = s.d5 = s.d6 = s.d7 = GPIO_NUM_NC;
    s.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    *slot = s;
}

static void host_config(sdmmc_host_t *host, uint32_t khz)
{
    sdmmc_host_t h = SDMMC_HOST_DEFAULT();
    h.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    h.max_freq_khz = (int)khz;
    *host = h;
}

/* Mount FAT at one frequency. ESP_FAIL = card answered but no usable FAT. */
static esp_err_t mount_at(uint32_t khz)
{
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,      /* NEVER format the user's card */
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = SD_ALLOC_UNIT,
    };
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    host_config(&host, khz);
    slot_config(&slot);

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &card);
    if (err == ESP_OK) {
        s_card = card;
        s_freq_khz = card->real_freq_khz > 0 ? (uint32_t)card->real_freq_khz : khz;
    }
    return err;
}

/* Called with s_lock held. Tries preferred freq, then 20 MHz. */
static esp_err_t do_mount_locked(bool quiet)
{
    esp_log_level_t saved[QUIET_N];
    if (quiet) quiet_logs(true, saved);

    esp_err_t err = mount_at(s_pref_khz);
    /* No card at all answers with a timeout during identification (400 kHz);
     * a lower bus clock cannot help then, so only fall back on other errors. */
    if (err != ESP_OK && err != ESP_FAIL && s_pref_khz != SD_FREQ_SLOW_KHZ &&
        !(quiet && err == ESP_ERR_TIMEOUT)) {
        if (!quiet) ESP_LOGW(TAG, "mount at %u kHz failed (%s), retrying at %u kHz",
                             (unsigned)s_pref_khz, esp_err_to_name(err), (unsigned)SD_FREQ_SLOW_KHZ);
        err = mount_at(SD_FREQ_SLOW_KHZ);
        if (err == ESP_OK) s_pref_khz = SD_FREQ_SLOW_KHZ;
    }

    if (quiet) quiet_logs(false, saved);

    if (err == ESP_OK) {
        s_state = SD_STATE_MOUNTED;
        s_fail_cnt = 0;
        ESP_LOGI(TAG, "mounted %s: %s, %u MB, %u kHz", SD_MOUNT_POINT,
                 s_card->cid.name, (unsigned)(((uint64_t)s_card->csd.capacity *
                 s_card->csd.sector_size) >> 20), (unsigned)s_freq_khz);
    } else if (err == ESP_FAIL) {
        s_state = SD_STATE_ERROR;
        ESP_LOGW(TAG, "card present but no FAT volume (exFAT/unformatted?) - not formatting");
    } else {
        s_state = SD_STATE_ABSENT;
        if (!quiet) ESP_LOGI(TAG, "no SD card (%s)", esp_err_to_name(err));
    }
    s_last_probe = xTaskGetTickCount();
    return err;
}

/* Called with s_lock held. */
static void unmount_locked(void)
{
    if (s_card) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        if (err != ESP_OK) ESP_LOGW(TAG, "unmount: %s", esp_err_to_name(err));
        s_card = NULL;
    }
    s_freq_khz = 0;
}

/* Bare bus probe without mounting (used while EJECTED to notice removal). */
static bool probe_card_present_locked(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    host_config(&host, SD_FREQ_SLOW_KHZ);
    slot_config(&slot);

    esp_log_level_t saved[QUIET_N];
    quiet_logs(true, saved);
    bool present = false;
    if (host.init() == ESP_OK) {
        if (sdmmc_host_init_slot(host.slot, &slot) == ESP_OK) {
            sdmmc_card_t *card = heap_caps_calloc(1, sizeof(*card), MALLOC_CAP_DEFAULT);
            if (card) {
                present = (sdmmc_card_init(&host, card) == ESP_OK);
                free(card);
            }
        }
        sdmmc_host_deinit();        /* we are the only SDMMC user */
    }
    quiet_logs(false, saved);
    return present;
}

static uint32_t ms_since(TickType_t t)
{
    return (uint32_t)((xTaskGetTickCount() - t) * portTICK_PERIOD_MS);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

esp_err_t sd_card_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    if (!s_io_mutex) s_io_mutex = xSemaphoreCreateMutexStatic(&s_io_mutex_storage);
    if (!s_lock || !s_io_mutex) return ESP_ERR_NO_MEM;
    if (!s_io_buf) {
        s_io_buf = heap_caps_malloc(SD_IO_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_io_buf) ESP_LOGW(TAG, "no internal DMA RAM for bounce buffer; PSRAM I/O will be slow");
    }

    lock();
    s_inited = true;
    if (s_state == SD_STATE_MOUNTED) {
        unlock();
        return ESP_OK;
    }
    esp_err_t err = do_mount_locked(false);
    unlock();

    if (err == ESP_OK) ensure_directories();
    return err;
}

void sd_card_poll(void)
{
    if (!s_lock || !s_inited) return;
    bool mounted_now = false;

    lock();
    switch (s_state) {
    case SD_STATE_MOUNTED:
        if (s_card && sdmmc_get_status(s_card) == ESP_OK) {
            s_fail_cnt = 0;
        } else if (++s_fail_cnt >= SD_REMOVE_FAILS) {
            ESP_LOGW(TAG, "card removed");
            s_state = SD_STATE_ABSENT;          /* stop new users first */
            vTaskDelay(pdMS_TO_TICKS(100));     /* let in-flight I/O fail out */
            unmount_locked();
            s_fail_cnt = 0;
            s_last_probe = xTaskGetTickCount();
        }
        break;
    case SD_STATE_ABSENT:
        if (ms_since(s_last_probe) >= SD_PROBE_ABSENT_MS) {
            mounted_now = (do_mount_locked(true) == ESP_OK);
        }
        break;
    case SD_STATE_ERROR:
        if (ms_since(s_last_probe) >= SD_PROBE_ERROR_MS) {
            esp_err_t err = do_mount_locked(true);
            mounted_now = (err == ESP_OK);
            if (err != ESP_OK && err != ESP_FAIL) ESP_LOGI(TAG, "unusable card removed");
        }
        break;
    case SD_STATE_EJECTED:
        if (ms_since(s_last_probe) >= SD_PROBE_ABSENT_MS) {
            s_last_probe = xTaskGetTickCount();
            if (probe_card_present_locked()) {
                s_fail_cnt = 0;
            } else if (++s_fail_cnt >= SD_REMOVE_FAILS) {
                ESP_LOGI(TAG, "ejected card pulled; waiting for insertion");
                s_state = SD_STATE_ABSENT;
                s_fail_cnt = 0;
            }
        }
        break;
    default:
        break;
    }
    unlock();

    if (mounted_now) ensure_directories();
}

uint32_t sd_card_poll_wait_ms(void)
{
    uint32_t interval_ms;
    uint32_t elapsed_ms;
    uint32_t wait_ms;

    /* A concurrent eject can only shorten this hint to 1 s; no lock is needed
     * on the usual mounted path. Non-mounted deadlines need a coherent pair. */
    if (!s_lock || !s_inited || s_state == SD_STATE_MOUNTED) return 1000;
    lock();
    switch (s_state) {
    case SD_STATE_MOUNTED: interval_ms = 1000; break;
    case SD_STATE_ERROR:   interval_ms = SD_PROBE_ERROR_MS; break;
    case SD_STATE_ABSENT:
    case SD_STATE_EJECTED: interval_ms = SD_PROBE_ABSENT_MS; break;
    default:               interval_ms = 1000; break;
    }
    if (s_state == SD_STATE_MOUNTED) {
        wait_ms = interval_ms;
    } else {
        elapsed_ms = ms_since(s_last_probe);
        wait_ms = elapsed_ms >= interval_ms ? 1 : interval_ms - elapsed_ms;
    }
    unlock();
    return wait_ms;
}

esp_err_t sd_card_eject(void)
{
    /* Cheap early-out: callable from the LVGL task without waiting on a probe. */
    if (!s_lock || s_state != SD_STATE_MOUNTED) return ESP_ERR_INVALID_STATE;
    lock();
    if (s_state != SD_STATE_MOUNTED) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_state = SD_STATE_EJECTED;
    unmount_locked();
    s_fail_cnt = 0;
    s_last_probe = xTaskGetTickCount();
    unlock();
    ESP_LOGI(TAG, "ejected; safe to remove");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

sd_state_t  sd_card_get_state(void)       { return s_state; }
bool        sd_card_is_mounted(void)      { return s_state == SD_STATE_MOUNTED; }
uint32_t    sd_card_get_freq_khz(void)    { return sd_card_is_mounted() ? s_freq_khz : 0; }

uint64_t sd_card_get_total_bytes(void)
{
    if (!sd_card_is_mounted()) return 0;
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK) return total;
    sdmmc_card_t *c = s_card;
    return c ? (uint64_t)c->csd.capacity * c->csd.sector_size : 0;
}

uint64_t sd_card_get_free_bytes(void)
{
    if (!sd_card_is_mounted()) return 0;
    uint64_t total = 0, free_b = 0;
    return esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK ? free_b : 0;
}

static esp_err_t ensure_directories(void)
{
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    static const char *const dirs[] = {
        SD_OBD_ROOT, SD_DIR_IMPORT, SD_DIR_THEMES, SD_DIR_VIDEO,
        SD_DIR_BOOT, SD_DIR_CFG, SD_DIR_CATALOG, SD_DIR_EXPORT,
    };
    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (sd_card_mkdir(dirs[i]) != ESP_OK) {
            ESP_LOGW(TAG, "cannot create %s", dirs[i]);
            ret = ESP_FAIL;
        }
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Bounce buffer                                                       */
/* ------------------------------------------------------------------ */

uint8_t *sd_card_io_buf_take(TickType_t wait)
{
    if (!s_io_buf || !s_io_mutex) return NULL;
    return xSemaphoreTake(s_io_mutex, wait) == pdTRUE ? s_io_buf : NULL;
}

void sd_card_io_buf_give(void)
{
    if (s_io_mutex && xSemaphoreGetMutexHolder(s_io_mutex) == xTaskGetCurrentTaskHandle()) {
        xSemaphoreGive(s_io_mutex);
    }
}

/* Direct transfer is fine for internal, word-aligned memory. If this task
 * already holds the bounce buffer (e.g. it passes the buffer itself, or nests),
 * go direct too: the driver then bounces per sector, slow but deadlock-free. */
static bool use_direct(const void *p)
{
    if (!s_io_buf) return true;
    if (esp_ptr_dma_capable(p) && (((uintptr_t)p & 3u) == 0)) return true;
    return xSemaphoreGetMutexHolder(s_io_mutex) == xTaskGetCurrentTaskHandle();
}

#if CONFIG_OBD_PERF_STATS
static size_t sd_card_read_impl(FILE *f, void *dst, size_t len);

size_t sd_card_read(FILE *f, void *dst, size_t len)
{
    PERF_BEGIN(perf_io_start);
    size_t transferred = sd_card_read_impl(f, dst, len);
    PERF_END(PERF_SD_READ, perf_io_start, (uint32_t)transferred);
    return transferred;
}

static size_t sd_card_read_impl(FILE *f, void *dst, size_t len)
#else
size_t sd_card_read(FILE *f, void *dst, size_t len)
#endif
{
    if (!f || !dst || len == 0) return 0;
    if (use_direct(dst)) return fread(dst, 1, len, f);

    uint8_t *bb = sd_card_io_buf_take(portMAX_DELAY);
    if (!bb) return fread(dst, 1, len, f);
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > SD_IO_BUF_SIZE) n = SD_IO_BUF_SIZE;
        size_t got = fread(bb, 1, n, f);
        memcpy((uint8_t *)dst + done, bb, got);
        done += got;
        if (got < n) break;
    }
    sd_card_io_buf_give();
    return done;
}

#if CONFIG_OBD_PERF_STATS
static size_t sd_card_write_impl(FILE *f, const void *src, size_t len);

size_t sd_card_write(FILE *f, const void *src, size_t len)
{
    PERF_BEGIN(perf_io_start);
    size_t transferred = sd_card_write_impl(f, src, len);
    PERF_END(PERF_SD_WRITE, perf_io_start, (uint32_t)transferred);
    return transferred;
}

static size_t sd_card_write_impl(FILE *f, const void *src, size_t len)
#else
size_t sd_card_write(FILE *f, const void *src, size_t len)
#endif
{
    if (!f || !src || len == 0) return 0;
    if (use_direct(src)) return fwrite(src, 1, len, f);

    uint8_t *bb = sd_card_io_buf_take(portMAX_DELAY);
    if (!bb) return fwrite(src, 1, len, f);
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > SD_IO_BUF_SIZE) n = SD_IO_BUF_SIZE;
        memcpy(bb, (const uint8_t *)src + done, n);
        size_t put = fwrite(bb, 1, n, f);
        done += put;
        if (put < n) break;
    }
    sd_card_io_buf_give();
    return done;
}

/* ------------------------------------------------------------------ */
/* Whole-file helpers                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t write_whole(const char *path, const void *data, size_t len, bool sync)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open %s: errno %d", path, errno);
        return ESP_FAIL;
    }
    setvbuf(f, NULL, _IONBF, 0);    /* large transfers go straight to FATFS */
    size_t put = len ? sd_card_write(f, data, len) : 0;
    bool ok = (put == len);
    if (ok && sync) {
        ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        ESP_LOGW(TAG, "write %s failed (%u/%u)", path, (unsigned)put, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_card_write_file(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    return write_whole(path, data, len, false);
}

esp_err_t sd_card_write_file_atomic(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    char part[SD_PATH_MAX];
    if (snprintf(part, sizeof(part), "%s.part", path) >= (int)sizeof(part)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = write_whole(part, data, len, true);
    if (err != ESP_OK) {
        unlink(part);
        return err;
    }
    return sd_card_rename(part, path);
}

esp_err_t sd_card_read_file(const char *path, void *buf, size_t buf_len, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!path || (!buf && buf_len)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    if ((size_t)st.st_size > buf_len) return ESP_ERR_INVALID_SIZE;

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    setvbuf(f, NULL, _IONBF, 0);
    size_t want = (size_t)st.st_size;
    size_t got = want ? sd_card_read(f, buf, want) : 0;
    fclose(f);
    if (got != want) return ESP_FAIL;
    if (out_len) *out_len = got;
    return ESP_OK;
}

esp_err_t sd_card_read_file_alloc(const char *path, size_t max_len, void **out, size_t *out_len)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (out_len) *out_len = 0;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    size_t sz = (size_t)st.st_size;
    if (sz > max_len) return ESP_ERR_INVALID_SIZE;

    /* +1 so text callers can NUL-terminate in place. */
    uint8_t *buf = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(sz + 1, MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t got = 0;
    esp_err_t err = sd_card_read_file(path, buf, sz, &got);
    if (err != ESP_OK) {
        heap_caps_free(buf);
        return err;
    }
    buf[got] = 0;
    *out = buf;
    if (out_len) *out_len = got;
    return ESP_OK;
}

esp_err_t sd_card_remove(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (unlink(path) == 0 || errno == ENOENT) return ESP_OK;
    return ESP_FAIL;
}

/* FAT rename cannot replace an existing destination. Keep the old file until
 * the new rename succeeds; a failed rollback leaves the backup for recovery.
 * FAT metadata updates still do not guarantee atomic power-loss recovery. */
esp_err_t sd_card_rename(const char *src, const char *dst)
{
    if (!src || !dst) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    struct stat st;
    if (stat(src, &st) != 0) return ESP_FAIL;
    if (strcmp(src, dst) == 0) return ESP_OK;
    if (stat(dst, &st) != 0) {
        if (errno != ENOENT || rename(src, dst) != 0) {
            ESP_LOGW(TAG, "rename %s -> %s: errno %d", src, dst, errno);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (S_ISDIR(st.st_mode)) return ESP_ERR_INVALID_ARG;

    char backup[SD_PATH_MAX];
    int n = snprintf(backup, sizeof(backup), "%s.obd-bak", dst);
    if (n < 0 || (size_t)n >= sizeof(backup)) return ESP_ERR_INVALID_SIZE;
    /* Never clobber an unrelated or unrecovered backup. */
    if (stat(backup, &st) == 0) return ESP_ERR_INVALID_STATE;
    if (errno != ENOENT || rename(dst, backup) != 0) {
        ESP_LOGW(TAG, "backup %s: errno %d", dst, errno);
        return ESP_FAIL;
    }
    if (rename(src, dst) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s: errno %d", src, dst, errno);
        if (rename(backup, dst) != 0) {
            ESP_LOGE(TAG, "restore %s failed; old file retained at %s", dst, backup);
        }
        return ESP_FAIL;
    }
    if (unlink(backup) != 0) ESP_LOGW(TAG, "cannot remove backup %s: errno %d", backup, errno);
    return ESP_OK;
}

esp_err_t sd_card_mkdir(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    if (mkdir(path, 0775) == 0 || errno == EEXIST) return ESP_OK;
    return ESP_FAIL;
}

bool sd_card_exists(const char *path)
{
    if (!path || !sd_card_is_mounted()) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t sd_card_append_line(const char *path, const char *line)
{
    if (!path || !line) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen(path, "ab");
    if (!f) return ESP_FAIL;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    bool ok = fwrite(line, 1, n, f) == n && fwrite("\r\n", 1, 2, f) == 2;
    if (fclose(f) != 0) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}

#else /* !CONFIG_OBD_SD_SLOT_SDMMC_1BIT: board without an SD slot */

esp_err_t   sd_card_init(void)               { ESP_LOGI(TAG, "no SD slot on this board"); return ESP_ERR_NOT_SUPPORTED; }
void        sd_card_poll(void)               { }
uint32_t    sd_card_poll_wait_ms(void)        { return 1000; }
esp_err_t   sd_card_eject(void)              { return ESP_ERR_NOT_SUPPORTED; }
sd_state_t  sd_card_get_state(void)          { return SD_STATE_DISABLED; }
bool        sd_card_is_mounted(void)         { return false; }
uint32_t    sd_card_get_freq_khz(void)       { return 0; }
uint64_t    sd_card_get_total_bytes(void)    { return 0; }
uint64_t    sd_card_get_free_bytes(void)     { return 0; }
uint8_t    *sd_card_io_buf_take(TickType_t wait) { (void)wait; return NULL; }
void        sd_card_io_buf_give(void)        { }
size_t sd_card_read(FILE *f, void *dst, size_t len)        { (void)f; (void)dst; (void)len; return 0; }
size_t sd_card_write(FILE *f, const void *src, size_t len) { (void)f; (void)src; (void)len; return 0; }
esp_err_t sd_card_write_file(const char *path, const void *data, size_t len)
{ (void)path; (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_write_file_atomic(const char *path, const void *data, size_t len)
{ (void)path; (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_read_file(const char *path, void *buf, size_t buf_len, size_t *out_len)
{ (void)path; (void)buf; (void)buf_len; if (out_len) *out_len = 0; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_read_file_alloc(const char *path, size_t max_len, void **out, size_t *out_len)
{ (void)path; (void)max_len; if (out) *out = NULL; if (out_len) *out_len = 0; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_remove(const char *path)                { (void)path; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_rename(const char *src, const char *dst) { (void)src; (void)dst; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_mkdir(const char *path)                 { (void)path; return ESP_ERR_NOT_SUPPORTED; }
bool      sd_card_exists(const char *path)                { (void)path; return false; }
esp_err_t sd_card_append_line(const char *path, const char *line)
{ (void)path; (void)line; return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_OBD_SD_SLOT_SDMMC_1BIT */
