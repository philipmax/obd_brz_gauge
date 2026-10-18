#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * upload_sink - the one streaming upload path shared by every HTTP upload
 * (/ota/firmware, /ota/theme, /ota/bootmedia and the /api/v1 uploads).
 *
 * An upload may span several HTTP requests (X-Offset / X-Last chunking). The
 * sink keeps the running size and SHA-256 across requests. Raw bodies for
 * memory-backed sinks are received directly into the final PSRAM payload;
 * SD/base64 bodies use a per-request staging buffer.
 *
 * Firmware accumulates in PSRAM before writing the inactive OTA slot.
 * Theme and boot media stream to SD .part files; commit follows verification.
 * Retired Flash media sink kinds return ESP_ERR_NOT_SUPPORTED.
 *
 * File sinks stream to SD (<path>.part, renamed on commit) or buffer in PSRAM
 * and go through config_part_write() for /config.
 *
 * A request that fails half way rolls the sink back to the offset it started
 * at, so the client can retry the same chunk.
 * -------------------------------------------------------------------------- */

/* SoftAP-stable maximum, verified on hardware. Do not raise. */
#define UPLOAD_RECV_CHUNK     (32 * 1024)
#define UPLOAD_PATH_MAX       128
/* First 16 KB of theme.bin is the manifest. */
#define UPLOAD_THEME_MANIFEST (16 * 1024)
/* Largest file accepted for the config partition. */
#define UPLOAD_CONFIG_MAX     (256 * 1024)

typedef enum {
    UPLOAD_SINK_NONE = 0,
    UPLOAD_SINK_FIRMWARE,     /* next OTA slot via esp_ota_* */
    UPLOAD_SINK_THEME,        /* retired Flash media kind: rejected */
    UPLOAD_SINK_BOOTMEDIA,    /* retired Flash media kind: rejected */
    UPLOAD_SINK_SD_FILE,      /* SD file: "<path>.part", renamed on commit */
    UPLOAD_SINK_CONFIG_FILE,  /* config LittleFS file (PSRAM, config_part_write) */
    UPLOAD_SINK_MEMORY,       /* PSRAM buffer only; the caller consumes buf */
} upload_sink_kind_t;

typedef struct {
    upload_sink_kind_t kind;
    uint32_t expected;        /* total payload bytes */
    uint32_t received;        /* payload bytes accepted so far */
    uint32_t manifest_size;   /* BOOTMEDIA: manifest bytes at the stream start */
    /* SD_FILE: at logical offset pad_at, pad the file with 0xFF up to file
     * offset pad_to (legacy bootmedia image: manifest padded to 4 KB). */
    uint32_t pad_at;
    uint32_t pad_to;
    bool     has_sha;
    uint8_t  sha_expected[32];
    bool     sha_running;
    mbedtls_sha256_context sha;
    uint8_t *buf;             /* PSRAM payload (firmware / config / memory kinds) */
    FILE    *fp;              /* SD_FILE */
    uint32_t file_pos;        /* SD_FILE: physical write position */
    uint32_t synced;          /* SD_FILE: bytes at the last fsync */
    const esp_partition_t *ota_part;   /* FIRMWARE */
    char     path[UPLOAD_PATH_MAX];    /* SD_FILE / CONFIG_FILE destination */
    const char *err;          /* static error text of the last failure */
} upload_sink_t;

/* 64 hex chars -> 32 bytes. false on wrong length or a non-hex character. */
bool upload_sha_from_hex(const char *hex, uint8_t out[32]);

/* Start an upload. sha may be NULL (no digest check). path is required for
 * SD_FILE / CONFIG_FILE. manifest_size is used by BOOTMEDIA only.
 * The sink must be zeroed or aborted before reuse. */
esp_err_t upload_sink_open(upload_sink_t *s, upload_sink_kind_t kind, uint32_t expected,
                           const uint8_t *sha, const char *path, uint32_t manifest_size);
bool      upload_sink_is_open(const upload_sink_t *s);
/* Append len payload bytes at offset s->received. */
esp_err_t upload_sink_write(upload_sink_t *s, const void *data, size_t len);
/* Receive this request's body (content_len bytes) and append it. base64:
 * the body is base64 text. Tolerates 3 consecutive recv timeouts. On
 * failure the sink is rolled back to where this request started. */
esp_err_t upload_sink_recv(upload_sink_t *s, httpd_req_t *req, bool base64);
/* Everything received and the digest matches. Writes nothing. */
esp_err_t upload_sink_verify(upload_sink_t *s);
/* Durable commit (flash write / rename / config write); call after verify.
 *   FIRMWARE   esp_ota_begin/write/end + set boot partition
 *   THEME      retired; ESP_ERR_NOT_SUPPORTED
 *   BOOTMEDIA  retired; ESP_ERR_NOT_SUPPORTED
 *   SD_FILE    fsync + rename over the destination
 *   CONFIG     config_part_write (mirrored to SD)
 *   MEMORY     nothing; buf[expected] is a NUL terminator */
esp_err_t upload_sink_commit(upload_sink_t *s);
/* Free buffers, close and delete a .part file; the sink is zeroed. */
void      upload_sink_abort(upload_sink_t *s);


#ifdef __cplusplus
}
#endif
