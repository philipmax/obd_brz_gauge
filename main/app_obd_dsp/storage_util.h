#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * storage_util - helpers shared by the storage jobs (theme library, inbox
 * import, SD import, first-boot migration) and bounded theme SD reads.
 * LVGL callers must use a bounded lock wait and read only required segments.
 * -------------------------------------------------------------------------- */

#define STORAGE_THEME_MANIFEST_SIZE  16384u  /* theme.bin [0,16 KB) = JSON manifest */
#define STORAGE_THEME_ID_LEN         65      /* editor ids are <= 64 chars */
#define STORAGE_THEME_KEY_LEN        65      /* sanitized id = SD directory name */
#define STORAGE_THEME_FILE           "theme.bin"

/* ---- job lock ----------------------------------------------------------
 * Serializes SD theme reads and jobs that modify SD / config (inbox import,
 * SD import, theme selection, migration). Recursive. FATFS runs with FF_FS_LOCK=0,
 * so two jobs must never rename/unlink files the other has open. */
bool storage_job_lock(TickType_t wait);
void storage_job_unlock(void);
bool storage_job_busy(void);

/* ---- theme.bin ----------------------------------------------------------- */
typedef struct {
    char     id[STORAGE_THEME_ID_LEN];
    char     name[64];
    char     version[16];
    char     author[48];
    uint32_t used_len;   /* max(offset+size) over assets and page layouts, >= 16 KB */
} storage_theme_meta_t;

/* Theme id -> FAT-safe directory name under SD_DIR_THEMES: keeps
 * [A-Za-z0-9._-], maps anything else to '_', drops leading/trailing dots,
 * prefixes DOS device names (CON, COM1...) with '_'; "theme" when empty.
 * Media/theme code must use this to find /OBD/THEMES/<theme_key>/. */
void storage_theme_key(const char *theme_id, char *out, size_t out_len);

/* Parse a theme.bin manifest without building a JSON tree (no heap).
 * blob_cap = bytes that exist after it (partition or file size).
 * ESP_ERR_NOT_FOUND        empty slot (first byte 0xFF or 0x00)
 * ESP_ERR_NOT_SUPPORTED    schema_version != "1.0"
 * ESP_ERR_INVALID_SIZE     an asset/layout lies outside [16 KB, blob_cap)
 * ESP_ERR_INVALID_RESPONSE malformed JSON / missing theme.id */
esp_err_t storage_theme_manifest_parse(const uint8_t *buf, size_t len, uint32_t blob_cap,
                                       storage_theme_meta_t *out);
/* Same, reading theme_0 / an SD file. */
esp_err_t storage_theme_meta_from_flash(storage_theme_meta_t *out);
esp_err_t storage_theme_meta_from_file(const char *path, storage_theme_meta_t *out);

/* ---- legacy bootmedia image [manifest 4 KB][delta_varint stream] ---------
 * Parses the manifest and walks frame_count frames to find the real stream
 * length (binary_size is usually absent). abs_addr = absolute flash address
 * of the manifest, avail = bytes that may belong to the image.
 * Returns ESP_ERR_NOT_FOUND for an empty slot (first byte not a-z),
 * ESP_ERR_INVALID_RESPONSE for a bad manifest or a stream that is corrupt
 * or longer than avail. image_len includes the 4 KB manifest. */
esp_err_t storage_bootmedia_probe(uint32_t abs_addr, uint32_t avail,
                                  uint32_t *image_len, uint32_t *crc);

/* ---- flash ---------------------------------------------------------------
 * abs_addr is an absolute flash address. Reads go through 1 MB mmap windows
 * (no SPI1 cache-disable stalls); esp_flash_read is the fallback. */
typedef bool (*storage_chunk_cb_t)(const uint8_t *data, size_t len, void *ctx);
esp_err_t storage_flash_foreach(uint32_t abs_addr, uint32_t len, storage_chunk_cb_t cb, void *ctx);
esp_err_t storage_flash_crc32(uint32_t abs_addr, uint32_t len, uint32_t *crc);

/* Copy flash -> SD file: writes "<dst>.part", fsync, rename. Parent dirs are
 * created. Goes through the shared bounce buffer one 16 KB chunk at a time.
 * crc (optional) = CRC32 of the bytes written. */
esp_err_t storage_copy_flash_to_sd(uint32_t abs_addr, uint32_t len, const char *dst, uint32_t *crc);

/* ---- SD -------------------------------------------------------------------- */
/* Read path [off, off+len) (len = 0: to EOF) in <= 16 KB chunks through the
 * bounce buffer. cb gets internal DMA-capable memory, valid only during the
 * call; returning false stops early (ESP_OK). ESP_ERR_INVALID_SIZE when the
 * file is shorter than off+len. */
esp_err_t storage_sd_file_foreach(const char *path, uint32_t off, uint32_t len,
                                  storage_chunk_cb_t cb, void *ctx);
/* mkdir -p for every parent directory of path (under SD_MOUNT_POINT). */
void      storage_sd_mkdir_parents(const char *path);
/* File size, -1 when missing or a directory. */
int64_t   storage_sd_file_size(const char *path);
bool      storage_has_suffix_ci(const char *s, const char *suffix);

/* Fresh PSRAM buffer (falls back to internal RAM); free with heap_caps_free(). */
void *storage_psram_alloc(size_t len);

#ifdef __cplusplus
}
#endif
