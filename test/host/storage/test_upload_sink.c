/* Real upload sink with deterministic file/network/crypto failure injection.
 * The fake hash checks state handling only, not SHA-256 implementation. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app_obd_dsp/upload_sink.h"

static int test_fseek(FILE *, long, int);
static int test_fflush(FILE *);
static int test_fsync(int);
static size_t test_strlcpy(char *, const char *, size_t);
#define fseek test_fseek
#define fflush test_fflush
#define fsync test_fsync
#define strlcpy test_strlcpy
#include "../../../main/app_obd_dsp/upload_sink.c"
#undef fseek
#undef fflush
#undef fsync
#undef strlcpy

static bool fail_seek, fail_sync, short_write, fail_sha, fail_sha_init;
static const char *body;
static size_t body_pos, body_length, body_limit;
static bool fail_recv;
static unsigned renames;

static int test_fseek(FILE *f, long offset, int whence)
{ if (fail_seek) { errno = EIO; return -1; } return fseek(f, offset, whence); }
static int test_fflush(FILE *f) { return fflush(f); }
static int test_fsync(int fd) { if (fail_sync) { errno = EIO; return -1; } return fsync(fd); }
static size_t test_strlcpy(char *dst, const char *src, size_t cap)
{ size_t n = strlen(src); if (cap) { size_t copy = n < cap-1 ? n : cap-1; memcpy(dst, src, copy); dst[copy] = 0; } return n; }
void storage_sd_mkdir_parents(const char *path) { (void)path; }
bool sd_card_is_mounted(void) { return true; }
size_t sd_card_write(FILE *f, const void *data, size_t len)
{ if (short_write) return fwrite(data, 1, len ? len - 1 : 0, f); return fwrite(data, 1, len, f); }
esp_err_t sd_card_remove(const char *p) { return unlink(p) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL; }
esp_err_t sd_card_rename(const char *a, const char *b) { renames++; return rename(a,b) == 0 ? ESP_OK : ESP_FAIL; }
bool config_part_is_mounted(void) { return true; }
bool config_part_has_quota(size_t n) { (void)n; return true; }
esp_err_t config_part_write(const char *p, const void *data, size_t n) { (void)p; (void)data; (void)n; return ESP_OK; }
static esp_partition_t partition = { .size = 0x300000 };
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { (void)p; return &partition; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h) { (void)p; (void)n; *h=1; return ESP_OK; }
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *p, size_t n) { (void)h; (void)p; (void)n; return ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t h) { (void)h; return ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { (void)h; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { (void)p; return ESP_OK; }
void mbedtls_sha256_init(mbedtls_sha256_context *c) { c->value=0; }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
void mbedtls_sha256_clone(mbedtls_sha256_context *a, const mbedtls_sha256_context *b) { *a=*b; }
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int mode) { (void)mode; c->value=0; return fail_sha_init ? -1 : 0; }
int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *p, size_t n)
{ if (fail_sha) return -1; for (size_t i=0; i<n; i++) c->value += p[i]; return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char out[32])
{ memset(out, 0, 32); out[0]=(unsigned char)c->value; return 0; }
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *n, const unsigned char *p, size_t len)
{ (void)out; (void)cap; (void)n; (void)p; (void)len; return -1; }
int httpd_req_recv(httpd_req_t *req, char *out, size_t want)
{
    (void)req;
    if (fail_recv && body_pos >= body_limit) return -1;
    size_t n=body_length-body_pos; if (n>want) n=want;
    if (fail_recv && n>body_limit-body_pos) n=body_limit-body_pos;
    if (!n) return -1;
    memcpy(out, body+body_pos, n); body_pos+=n; return (int)n;
}

static void request(const char *data, size_t length, bool fail, size_t limit)
{ body=data; body_pos=0; body_length=length; fail_recv=fail; body_limit=limit; }
static void path_for(char *out, size_t cap)
{ snprintf(out, cap, SD_MOUNT_POINT "/case-%ld", (long)getpid()); }

int main(void)
{
    assert(mkdir(SD_MOUNT_POINT, 0700) == 0 || errno == EEXIST);
    char path[128]; path_for(path, sizeof(path));
    upload_sink_t s = {0};
    uint8_t wrong_sha[32] = {0};
    assert(upload_sink_open(&s, UPLOAD_SINK_MEMORY, 3, wrong_sha, NULL, 0) == ESP_OK);
    assert(upload_sink_write(&s, "abc", 3) == ESP_OK);
    assert(upload_sink_verify(&s) == ESP_ERR_INVALID_CRC);
    assert(upload_sink_verify(&s) == ESP_ERR_INVALID_STATE);
    assert(upload_sink_commit(&s) == ESP_ERR_INVALID_STATE);
    upload_sink_abort(&s);

    assert(upload_sink_open(&s, UPLOAD_SINK_SD_FILE, 6, NULL, path, 0) == ESP_OK);
    httpd_req_t req = {.content_len=6}; request("abcdef",6,true,3);
    assert(upload_sink_recv(&s,&req,false) != ESP_OK && !s.failed && s.received == 0);
    request("abcdef",6,false,0);
    assert(upload_sink_recv(&s,&req,false) == ESP_OK && upload_sink_verify(&s) == ESP_OK);
    assert(upload_sink_verify(&s) == ESP_OK && upload_sink_commit(&s) == ESP_OK);
    FILE *f=fopen(path,"rb"); assert(f); char contents[7]={0};
    assert(fread(contents,1,6,f)==6 && !strcmp(contents,"abcdef")); fclose(f); upload_sink_abort(&s);

    assert(upload_sink_open(&s, UPLOAD_SINK_SD_FILE, 6, NULL, path, 0) == ESP_OK);
    request("abcdef",6,true,3); fail_seek=true;
    assert(upload_sink_recv(&s,&req,false) != ESP_OK && s.failed);
    assert(upload_sink_write(&s,"abcdef",6) == ESP_ERR_INVALID_STATE);
    assert(upload_sink_verify(&s) == ESP_ERR_INVALID_STATE && upload_sink_commit(&s) == ESP_ERR_INVALID_STATE);
    fail_seek=false; upload_sink_abort(&s);

    assert(upload_sink_open(&s, UPLOAD_SINK_SD_FILE, 6, NULL, path, 0) == ESP_OK);
    short_write=true; assert(upload_sink_write(&s,"abcdef",6) != ESP_OK && s.failed);
    assert(upload_sink_commit(&s) == ESP_ERR_INVALID_STATE); short_write=false; upload_sink_abort(&s);

    assert(upload_sink_open(&s, UPLOAD_SINK_SD_FILE, 1024*1024, NULL, path, 0) == ESP_OK);
    uint8_t *large=calloc(1,1024*1024); assert(large); fail_sync=true;
    assert(upload_sink_write(&s,large,1024*1024) != ESP_OK && s.failed);
    fail_sync=false; free(large); upload_sink_abort(&s);

    f=fopen(path,"rb"); assert(f); memset(contents,0,sizeof(contents));
    assert(fread(contents,1,6,f)==6 && !strcmp(contents,"abcdef")); fclose(f);

    fail_sha_init=true;
    assert(upload_sink_open(&s, UPLOAD_SINK_SD_FILE, 3, NULL, path, 0) == ESP_FAIL);
    assert(!upload_sink_is_open(&s)); fail_sha_init=false;

    assert(upload_sink_open(&s, UPLOAD_SINK_MEMORY, 3, NULL, NULL, 0) == ESP_OK);
    fail_sha=true; assert(upload_sink_write(&s,"abc",3) != ESP_OK && s.failed);
    assert(upload_sink_verify(&s) == ESP_ERR_INVALID_STATE); fail_sha=false; upload_sink_abort(&s);
    assert(renames == 1); assert(unlink(path) == 0);
    return 0;
}
