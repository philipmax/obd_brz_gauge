#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "media/boot_obv_validate.h"
#include "media/media_jpeg.h"
#include "media/obv1_format.h"
#include "media/media_jpeg_selftest.inc"

/* Real container/JPEG headers, substituting only the hardware decoder boundary. */
struct media_jpeg { int unused; };
static struct media_jpeg decoder;
static unsigned opens, closes, begins, decodes;
static bool fail_begin, fail_decode;
esp_err_t media_jpeg_open(bool block, media_jpeg_t **out)
{
    assert(!block); opens++; *out = &decoder; return ESP_OK;
}
void media_jpeg_close(media_jpeg_t *j) { assert(j == &decoder); closes++; }
esp_err_t media_jpeg_begin(media_jpeg_t *j, const uint8_t *data, size_t len, media_jpeg_info_t *info)
{
    assert(j == &decoder && len > 4 && data[0] == 255);
    begins++;
    *info = (media_jpeg_info_t){.width = 360, .height = 360, .block_len = 368 * 368 * 2, .blocks = 1};
    return fail_begin ? ESP_FAIL : ESP_OK;
}
esp_err_t media_jpeg_decode_frame(media_jpeg_t *j, uint8_t *out, size_t cap)
{
    assert(j == &decoder && out && cap >= 368 * 368 * 2);
    decodes++;
    /* Exercise the padded output capacity required by the 4:2:0 MCU geometry. */
    memset(out, 0, cap);
    return fail_decode ? ESP_FAIL : ESP_OK;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    for (unsigned n = 0; n < 4; n++) p[n] = (uint8_t)(v >> (n * 8));
}
static uint8_t *build(size_t *length, unsigned frames, uint8_t sampling)
{
    const uint8_t *jpeg = sampling == OBV1_SUB_420 ? k_st_jpeg_420 : k_st_jpeg_422;
    size_t size = sampling == OBV1_SUB_420 ? sizeof(k_st_jpeg_420) : sizeof(k_st_jpeg_422);
    size_t data_off = 64 + frames * 16;
    *length = data_off + size * frames;
    uint8_t *blob = calloc(1, *length);
    assert(blob);
    memcpy(blob, "OBV1", 4);
    wr16(blob + 4, 2); wr16(blob + 6, 64); wr16(blob + 8, 360); wr16(blob + 10, 360);
    blob[14] = sampling; blob[15] = sampling == OBV1_SUB_422 ? OBV1_FLAG_BLOCK_SAFE : 0;
    wr32(blob + 16, frames); wr32(blob + 20, 64); wr32(blob + 24, (uint32_t)data_off);
    wr32(blob + 28, (uint32_t)(size * frames)); wr32(blob + 32, (uint32_t)size);
    wr32(blob + 36, frames * 600); wr32(blob + 48, 1000000);
    for (unsigned i = 0; i < frames; i++) {
        uint8_t *entry = blob + 64 + i * 16;
        wr32(entry, (uint32_t)(size * i)); wr32(entry + 4, (uint32_t)size);
        wr32(entry + 8, i * 600000); wr32(entry + 12, 600000);
        memcpy(blob + data_off + size * i, jpeg, size);
    }
    wr32(blob + 40, obv1_crc32(0, blob + 64, frames * 16));
    wr32(blob + 44, obv1_crc32(0, blob + data_off, size * frames));
    wr32(blob + 60, obv1_crc32(0, blob, 60));
    return blob;
}
static bool validate(const uint8_t *blob, size_t len, const char **reason)
{
    char path[] = "/tmp/boot-obv-validate-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    FILE *file = fdopen(fd, "wb"); assert(file);
    assert(fwrite(blob, 1, len, file) == len); assert(fclose(file) == 0);
    bool ok = boot_obv_validate_file(path, reason);
    assert(unlink(path) == 0);
    assert(opens == closes);
    return ok;
}
static size_t marker_at(uint8_t *blob, size_t len, size_t start, uint8_t marker)
{
    for (size_t n = start; n + 1 < len; n++) if (blob[n] == 255 && blob[n + 1] == marker) return n;
    assert(!"JPEG fixture marker missing"); return 0;
}
int main(void)
{
    size_t len; const char *reason;
    uint8_t *blob = build(&len, 3, OBV1_SUB_420);
    assert(validate(blob, len, &reason)); assert(reason == NULL);
    assert(begins == 3 && decodes == 2); /* all headers, full decode for first second */
    assert(!validate(blob, len - 1, &reason) && reason);
    uint8_t *extra = calloc(1, len + 1); assert(extra); memcpy(extra, blob, len);
    assert(!validate(extra, len + 1, &reason)); free(extra);

    /* Pixel data corruption with valid JPEG markers must still fail CRC. */
    blob[len - 4] ^= 1;
    assert(!validate(blob, len, &reason)); assert(strstr(reason, "CRC"));
    blob[len - 4] ^= 1;
    fail_decode = true;
    assert(!validate(blob, len, &reason)); assert(strstr(reason, "JPEG"));
    fail_decode = false; fail_begin = true;
    assert(!validate(blob, len, &reason)); fail_begin = false;
    free(blob);

    blob = build(&len, 1, OBV1_SUB_422);
    assert(validate(blob, len, &reason)); free(blob);

    blob = build(&len, 1, OBV1_SUB_420);
    size_t sof = marker_at(blob, len, 80, 0xc0);
    blob[sof + 1] = 0xc2; /* progressive masquerading as baseline */
    assert(!validate(blob, len, &reason)); assert(strstr(reason, "JPEG"));
    blob[sof + 1] = 0xc0; blob[sof + 11] = 0x21; /* sampling mismatch */
    assert(!validate(blob, len, &reason)); free(blob);

    blob = build(&len, 1, OBV1_SUB_420);
    size_t sos = marker_at(blob, len, 80, 0xda);
    blob[sos + 1] = 0xd9; /* SOF without a supported scan */
    assert(!validate(blob, len, &reason)); free(blob);
    printf("OBV2 install validation: malformed headers, CRC, sampling and decode failures rejected\n");
    return 0;
}
