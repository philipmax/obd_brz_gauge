#include "boot_obv_validate.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "media/media_jpeg.h"
#include "media/obv1_format.h"
#ifdef ESP_PLATFORM
#include "bsp_obd_dsp/sd_card.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define read_bytes sd_card_read
static void *buffer_alloc(size_t n) { return heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
#else
static size_t read_bytes(FILE *f, void *p, size_t n) { return fread(p, 1, n, f); }
static void *buffer_alloc(size_t n) { return malloc(n); }
#endif

static bool jpeg_sampling(const uint8_t *p, size_t n, uint8_t sampling, uint16_t *w, uint16_t *h)
{
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8 || p[n - 2] != 0xff || p[n - 1] != 0xd9) return false;
    size_t at = 2;
    bool sof = false;
    while (at + 4 <= n) {
        if (p[at++] != 0xff) return false;
        while (at < n && p[at] == 0xff) at++;
        if (at >= n) return false;
        uint8_t marker = p[at++];
        if (at + 2 > n) return false;
        size_t len = ((size_t)p[at] << 8) | p[at + 1];
        if (len < 2 || len > n - at) return false;
        if (marker == 0xda) return sof && len == 12 && p[at + 2] == 3;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (marker != 0xc0 || sof || len != 17 || p[at + 2] != 8 || p[at + 7] != 3 ||
                p[at + 9] != (sampling == OBV1_SUB_422 ? 0x21 : 0x22) ||
                p[at + 12] != 0x11 || p[at + 15] != 0x11) return false;
            *h = ((uint16_t)p[at + 3] << 8) | p[at + 4];
            *w = ((uint16_t)p[at + 5] << 8) | p[at + 6];
            sof = true;
        }
        at += len;
    }
    return false;
}

bool boot_obv_validate_file(const char *path, const char **reason)
{
    static const char *bad_header = "Invalid OBV2 header or timing index";
    static const char *bad_crc = "OBV2 CRC mismatch";
    static const char *bad_frame = "Frame is not supported baseline JPEG";
    if (reason) *reason = bad_header;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    uint8_t header[OBV1_HEADER_SIZE];
    uint8_t *index = NULL;
    uint8_t *frame = NULL;
    uint8_t *pixels = NULL;
    media_jpeg_t *decoder = NULL;
    obv1_info_t info;
    if (fseek(f, 0, SEEK_END) != 0) goto done;
    long length = ftell(f);
    if (length < (long)sizeof(header) || (uint64_t)length > 256u * 1024u * 1024u || fseek(f, 0, SEEK_SET) != 0) goto done;
    if (read_bytes(f, header, sizeof(header)) != sizeof(header) ||
        obv1_parse_header(header, sizeof(header), (uint64_t)length, OBV1_FRAME_MAX, &info) != OBV1_OK ||
        info.version != OBV2_VERSION ||
        (uint64_t)info.data_offset + info.data_size != (uint64_t)length) goto done;
    size_t index_size = (size_t)info.frame_count * OBV2_INDEX_ENTRY;
    index = buffer_alloc(index_size);
    frame = buffer_alloc(info.max_frame_size);
    size_t pixel_cap = (size_t)((info.width + 15u) & ~15u) * ((info.height + 15u) & ~15u) * 2u;
    pixels = buffer_alloc(pixel_cap);
    if (!index || !frame || !pixels || fseek(f, (long)info.index_offset, SEEK_SET) != 0 ||
        read_bytes(f, index, index_size) != index_size || obv1_check_index(&info, index, index_size) != OBV1_OK) {
        if (reason) *reason = bad_crc;
        goto done;
    }
    if (media_jpeg_open(false, &decoder) != ESP_OK) goto done;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < info.frame_count; i++) {
        uint32_t off, size;
        obv_index_frame(&info, index, i, &off, &size);
        uint64_t pos = (uint64_t)info.data_offset + off;
        if (pos > LONG_MAX || fseek(f, (long)pos, SEEK_SET) != 0 || read_bytes(f, frame, size) != size) goto done;
        crc = obv1_crc32(crc, frame, size);
        uint16_t w = 0, h = 0;
        media_jpeg_info_t decoded;
        if (!jpeg_sampling(frame, size, info.subsampling, &w, &h) || w != info.width || h != info.height ||
            media_jpeg_begin(decoder, frame, size, &decoded) != ESP_OK || decoded.width != info.width || decoded.height != info.height ||
            (obv_frame_time(index, i) < OBV2_TIMESCALE &&
             media_jpeg_decode_frame(decoder, pixels, pixel_cap) != ESP_OK)) {
            if (reason) *reason = bad_frame;
            goto done;
        }
#ifdef ESP_PLATFORM
        if (!(i % 8)) vTaskDelay(1);
#endif
    }
    if (crc != info.data_crc32) { if (reason) *reason = bad_crc; goto done; }
    ok = true;
    if (reason) *reason = NULL;
done:
    if (decoder) media_jpeg_close(decoder);
    free(index);
    free(frame);
    free(pixels);
    fclose(f);
    return ok;
}
