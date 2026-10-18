/*
 * media_jpeg.c — esp_new_jpeg 1.0.2 wrapper (see media_jpeg.h).
 */

#include "media/media_jpeg.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "media/obv1_format.h"

static const char *TAG = "media_jpeg";

struct media_jpeg {
    jpeg_dec_handle_t dec;
    jpeg_dec_io_t     io;
    bool              block;
    bool              ready;       /* header parsed, frame not finished */
    uint16_t          width;
    uint16_t          height;
    uint16_t          mcu_rows;
    uint32_t          block_len;
    uint32_t          blocks;
    uint32_t          block_idx;
    uint32_t          lines_done;
};

static esp_err_t map_err(jpeg_error_t e)
{
    switch (e) {
    case JPEG_ERR_OK:            return ESP_OK;
    case JPEG_ERR_NO_MEM:        return ESP_ERR_NO_MEM;
    case JPEG_ERR_INVALID_PARAM: return ESP_ERR_INVALID_ARG;
    case JPEG_ERR_NO_MORE_DATA:  return ESP_ERR_INVALID_SIZE;
    case JPEG_ERR_UNSUPPORT_FMT:
    case JPEG_ERR_UNSUPPORT_STD: return ESP_ERR_NOT_SUPPORTED;
    default:                     return ESP_FAIL;
    }
}

esp_err_t media_jpeg_open(bool block_mode, media_jpeg_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    media_jpeg_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!j) return ESP_ERR_NO_MEM;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    cfg.block_enable = block_mode;
    jpeg_error_t e = jpeg_dec_open(&cfg, &j->dec);
    if (e != JPEG_ERR_OK || !j->dec) {
        ESP_LOGE(TAG, "jpeg_dec_open failed (%d)", (int)e);
        heap_caps_free(j);
        return e != JPEG_ERR_OK ? map_err(e) : ESP_ERR_NO_MEM;
    }
    j->block = block_mode;
    *out = j;
    return ESP_OK;
}

void media_jpeg_close(media_jpeg_t *j)
{
    if (!j) return;
    if (j->dec) jpeg_dec_close(j->dec);
    heap_caps_free(j);
}

esp_err_t media_jpeg_begin(media_jpeg_t *j, const uint8_t *data, size_t len,
                           media_jpeg_info_t *info)
{
    if (!j || !data || len < 4 || len > INT32_MAX) return ESP_ERR_INVALID_ARG;
    j->ready = false;
    memset(&j->io, 0, sizeof(j->io));
    j->io.inbuf = (uint8_t *)data;
    j->io.inbuf_len = (int)len;

    jpeg_dec_header_info_t hdr = {0};
    jpeg_error_t e = jpeg_dec_parse_header(j->dec, &j->io, &hdr);
    if (e != JPEG_ERR_OK) return map_err(e);
    if (!hdr.width || !hdr.height) return ESP_ERR_INVALID_SIZE;

    int outlen = 0, count = 0;
    e = jpeg_dec_get_outbuf_len(j->dec, &outlen);
    if (e == JPEG_ERR_OK) e = jpeg_dec_get_process_count(j->dec, &count);
    if (e != JPEG_ERR_OK) return map_err(e);
    if (outlen <= 0 || count <= 0) return ESP_ERR_INVALID_SIZE;

    j->width = hdr.width;
    j->height = hdr.height;
    j->block_len = (uint32_t)outlen;
    j->blocks = (uint32_t)count;
    j->mcu_rows = 0;
    if (j->block) {
        /* 8- or 16-line MCU rows; anything else means block mode is unusable */
        uint32_t line = (uint32_t)hdr.width * 2u;
        if (j->blocks == (hdr.height + 7u) / 8u) j->mcu_rows = 8;
        else if (j->blocks == (hdr.height + 15u) / 16u) j->mcu_rows = 16;
        if (!j->mcu_rows || j->block_len < line * j->mcu_rows) return ESP_ERR_NOT_SUPPORTED;
    } else if (j->blocks != 1) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    j->block_idx = 0;
    j->lines_done = 0;
    j->ready = true;
    if (info) {
        info->width = j->width;
        info->height = j->height;
        info->mcu_rows = j->mcu_rows;
        info->block_len = j->block_len;
        info->blocks = j->blocks;
    }
    return ESP_OK;
}

esp_err_t media_jpeg_next_block(media_jpeg_t *j, uint8_t *out, uint16_t *lines)
{
    if (lines) *lines = 0;
    if (!j || !out || ((uintptr_t)out & 15u)) return ESP_ERR_INVALID_ARG;
    if (!j->ready || !j->block || j->block_idx >= j->blocks) return ESP_ERR_INVALID_STATE;
    j->io.outbuf = out;
    jpeg_error_t e = jpeg_dec_process(j->dec, &j->io);
    if (e != JPEG_ERR_OK) {
        j->ready = false;
        return map_err(e);
    }
    uint32_t line = (uint32_t)j->width * 2u;
    if (j->io.out_size <= 0 || ((uint32_t)j->io.out_size % line) != 0) {
        j->ready = false;
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t n = (uint32_t)j->io.out_size / line;
    uint32_t left = j->height - j->lines_done;
    if (n > left) n = left;
    j->lines_done += n;
    j->block_idx++;
    if (j->block_idx >= j->blocks) j->ready = false;
    if (lines) *lines = (uint16_t)n;
    return ESP_OK;
}

esp_err_t media_jpeg_decode_frame(media_jpeg_t *j, uint8_t *out, size_t out_cap)
{
    if (!j || !out || ((uintptr_t)out & 15u)) return ESP_ERR_INVALID_ARG;
    if (!j->ready || j->block) return ESP_ERR_INVALID_STATE;
    if (out_cap < j->block_len) return ESP_ERR_INVALID_SIZE;
    j->io.outbuf = out;
    jpeg_error_t e = jpeg_dec_process(j->dec, &j->io);
    j->ready = false;
    return map_err(e);
}

/* ---------------------------------------------------------------- self-test */

#include "media/media_jpeg_selftest.inc"

#define ST_TOL_888   24   /* probe tolerance per 8-bit channel */
#define ST_TOL_565   2    /* frame vs block, per 565 channel */

enum { ST_UNTESTED = 0, ST_PASS, ST_FAIL };

typedef struct {
    uint8_t frame;
    uint8_t block;
} st_result_t;

static st_result_t s_st[2];   /* [0] 4:2:2, [1] 4:2:0 */
static portMUX_TYPE s_st_lock = portMUX_INITIALIZER_UNLOCKED;

static inline uint16_t px_at(const uint8_t *buf, uint32_t x, uint32_t y)
{
    const uint8_t *p = buf + ((size_t)y * MJ_ST_SIZE + x) * 2u;
    return (uint16_t)((p[0] << 8) | p[1]);   /* RGB565 big-endian */
}

static bool probe_ok(uint16_t c, const uint8_t want[3])
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    int rgb[3] = {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
    for (int i = 0; i < 3; i++) {
        int d = rgb[i] - want[i];
        if (d > ST_TOL_888 || d < -ST_TOL_888) return false;
    }
    return true;
}

/* Cell centres plus the half-MCU band; returns the number of failed probes. */
static int check_probes(const uint8_t *buf)
{
    int bad = 0;
    for (int cy = 0; cy < MJ_ST_CELLS; cy++) {
        for (int cx = 0; cx < MJ_ST_CELLS; cx++) {
            uint32_t x = (uint32_t)(cx * MJ_ST_CELL + MJ_ST_CELL / 2);
            uint32_t y = (uint32_t)(cy * MJ_ST_CELL + MJ_ST_CELL / 2);
            if (!probe_ok(px_at(buf, x, y), k_st_palette[k_st_cells[cy][cx]])) bad++;
        }
    }
    for (uint32_t x = 4; x < MJ_ST_SIZE; x += 8) {
        if (!probe_ok(px_at(buf, x, MJ_ST_BAND_Y + 4), k_st_band)) bad++;
    }
    return bad;
}

static uint32_t compare_565(const uint8_t *a, const uint8_t *b, size_t pixels)
{
    uint32_t bad = 0;
    for (size_t i = 0; i < pixels; i++) {
        uint16_t ca = (uint16_t)((a[2 * i] << 8) | a[2 * i + 1]);
        uint16_t cb = (uint16_t)((b[2 * i] << 8) | b[2 * i + 1]);
        if (ca == cb) continue;
        int dr = ((ca >> 11) & 0x1F) - ((cb >> 11) & 0x1F);
        int dg = ((ca >> 5) & 0x3F) - ((cb >> 5) & 0x3F);
        int db = (ca & 0x1F) - (cb & 0x1F);
        if (dr > ST_TOL_565 || dr < -ST_TOL_565 || dg > ST_TOL_565 || dg < -ST_TOL_565 ||
            db > ST_TOL_565 || db < -ST_TOL_565) {
            bad++;
        }
    }
    return bad;
}

/* Frame-mode decode of the whole picture into out. */
static esp_err_t st_decode_frame(const uint8_t *jpg, size_t len, uint8_t *out, size_t cap)
{
    media_jpeg_t *j = NULL;
    esp_err_t err = media_jpeg_open(false, &j);
    if (err != ESP_OK) return err;
    media_jpeg_info_t info;
    err = media_jpeg_begin(j, jpg, len, &info);
    if (err == ESP_OK && (info.width != MJ_ST_SIZE || info.height != MJ_ST_SIZE)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) err = media_jpeg_decode_frame(j, out, cap);
    media_jpeg_close(j);
    return err;
}

/* Block-mode decode, one MCU row after another into out (needs height lines
 * plus one block_len of slack). */
static esp_err_t st_decode_blocks(media_jpeg_t *j, const uint8_t *jpg, size_t len,
                                  uint8_t *out, size_t cap)
{
    media_jpeg_info_t info;
    esp_err_t err = media_jpeg_begin(j, jpg, len, &info);
    if (err != ESP_OK) return err;
    if (info.width != MJ_ST_SIZE || info.height != MJ_ST_SIZE) return ESP_ERR_INVALID_SIZE;
    size_t line = (size_t)info.width * 2u;
    uint32_t done = 0;
    for (uint32_t b = 0; b < info.blocks; b++) {
        if ((size_t)done * line + info.block_len > cap) return ESP_ERR_INVALID_SIZE;
        uint16_t lines = 0;
        err = media_jpeg_next_block(j, out + (size_t)done * line, &lines);
        if (err != ESP_OK) return err;
        done += lines;
    }
    return done == info.height ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static st_result_t run_self_test(int slot)
{
    st_result_t r = {ST_FAIL, ST_FAIL};
    const uint8_t *src = slot == 0 ? k_st_jpeg_422 : k_st_jpeg_420;
    size_t len = slot == 0 ? sizeof(k_st_jpeg_422) : sizeof(k_st_jpeg_420);
    const char *name = slot == 0 ? "4:2:2" : "4:2:0";
    const size_t frame = (size_t)MJ_ST_SIZE * MJ_ST_SIZE * 2u;
    const size_t slack = (size_t)MJ_ST_SIZE * 2u * 16u;   /* one 16-line block */
    const size_t jpg_off = (frame + frame + slack + 15u) & ~(size_t)15u;

    /* One PSRAM block: [frame A][frame B + slack][jpeg copy] */
    uint8_t *mem = heap_caps_aligned_alloc(16, jpg_off + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) {
        ESP_LOGW(TAG, "self-test %s: no PSRAM", name);
        return r;
    }
    uint8_t *a = mem, *b = mem + frame, *jpg = mem + jpg_off;
    memcpy(jpg, src, len);   /* playback decodes from the PSRAM ring too */

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = st_decode_frame(jpg, len, a, frame);
    int64_t t_frame = esp_timer_get_time() - t0;
    int bad_a = err == ESP_OK ? check_probes(a) : -1;
    if (err == ESP_OK && bad_a == 0) r.frame = ST_PASS;

    int64_t t_block = 0;
    uint32_t mismatch = 0;
    int bad_b = -1;
    media_jpeg_t *j = NULL;
    esp_err_t berr = media_jpeg_open(true, &j);
    for (int pass = 0; berr == ESP_OK && pass < 2; pass++) {   /* 2nd pass: handle reuse */
        memset(b, 0, frame);
        t0 = esp_timer_get_time();
        berr = st_decode_blocks(j, jpg, len, b, frame + slack);
        t_block = esp_timer_get_time() - t0;
        if (berr != ESP_OK) break;
        bad_b = check_probes(b);
        mismatch = r.frame == ST_PASS ? compare_565(a, b, frame / 2u) : 0;
        if (bad_b || mismatch || r.frame != ST_PASS) break;
    }
    media_jpeg_close(j);
    if (berr == ESP_OK && bad_b == 0 && mismatch == 0 && r.frame == ST_PASS) r.block = ST_PASS;

    ESP_LOGI(TAG, "self-test %s: frame %s (%d ms, err 0x%x, bad probes %d), "
             "block %s (%d ms, err 0x%x, bad probes %d, mismatch %u px)",
             name, r.frame == ST_PASS ? "ok" : "FAIL", (int)(t_frame / 1000), (unsigned)err, bad_a,
             r.block == ST_PASS ? "ok" : "FAIL", (int)(t_block / 1000), (unsigned)berr, bad_b,
             (unsigned)mismatch);
    heap_caps_free(mem);
    return r;
}

static st_result_t cached_test(int slot)
{
    portENTER_CRITICAL(&s_st_lock);
    st_result_t r = s_st[slot];
    portEXIT_CRITICAL(&s_st_lock);
    if (r.frame != ST_UNTESTED) return r;
    r = run_self_test(slot);   /* concurrent first calls just run it twice */
    portENTER_CRITICAL(&s_st_lock);
    s_st[slot] = r;
    portEXIT_CRITICAL(&s_st_lock);
    return r;
}

bool media_jpeg_block_ok(uint8_t subsampling)
{
    if (subsampling == OBV1_SUB_422) return cached_test(0).block == ST_PASS;
    if (subsampling == OBV1_SUB_420) return cached_test(1).block == ST_PASS;
    return false;
}

