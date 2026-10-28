#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"

#define SD_PATH_MAX 264
static const char *TAG = "test";
typedef struct { char path[300]; int kind; } item_t;
static item_t items[4];
static int rename_calls, fail_rename_first, fail_rename_second;
static bool fail_unlink, mounted = true, fail_stat;

static item_t *find_item(const char *path)
{
    for (unsigned i=0; i<4; i++) if (items[i].kind && !strcmp(items[i].path,path)) return &items[i];
    return NULL;
}
static void add_item(const char *path, int kind)
{
    for (unsigned i=0; i<4; i++) if (!items[i].kind) {
        snprintf(items[i].path,sizeof(items[i].path),"%s",path); items[i].kind=kind; return;
    }
    assert(false);
}
static int fake_stat(const char *path, struct stat *out)
{
    if (fail_stat) { errno=EIO; return -1; }
    item_t *item=find_item(path);
    if (!item) { errno=ENOENT; return -1; }
    memset(out,0,sizeof(*out)); out->st_mode=item->kind == 2 ? 0040000 : 0100000; return 0;
}
static int fake_rename(const char *src, const char *dst)
{
    rename_calls++;
    if (rename_calls==fail_rename_first || rename_calls==fail_rename_second) { errno=EIO; return -1; }
    item_t *item=find_item(src);
    if (!item) { errno=ENOENT; return -1; }
    if (find_item(dst)) { errno=EEXIST; return -1; }
    snprintf(item->path,sizeof(item->path),"%s",dst); return 0;
}
static int fake_unlink(const char *path)
{
    if (fail_unlink) { errno=EIO; return -1; }
    item_t *item=find_item(path); if (!item) { errno=ENOENT; return -1; }
    item->kind=0; return 0;
}
static bool sd_card_is_mounted(void) { return mounted; }
#define stat(path, info) fake_stat(path, info)
#define rename(src, dst) fake_rename(src, dst)
#define unlink(path) fake_unlink(path)
#include "sd_rename.inc"
#undef stat
#undef rename
#undef unlink

static void reset(void)
{
    memset(items,0,sizeof(items)); rename_calls=fail_rename_first=fail_rename_second=0;
    fail_unlink=fail_stat=false; mounted=true;
}
int main(void)
{
    reset(); add_item("dst",1);
    assert(sd_card_rename("missing","dst") == ESP_FAIL && find_item("dst"));
    add_item("src",1); assert(sd_card_rename("src","src") == ESP_OK && rename_calls==0);
    fail_stat=true; assert(sd_card_rename("src","dst") == ESP_FAIL && find_item("dst")); fail_stat=false;
    mounted=false; assert(sd_card_rename("src","dst") == ESP_ERR_INVALID_STATE); mounted=true;

    reset(); add_item("src",1);
    assert(sd_card_rename("src","dst") == ESP_OK && !find_item("src") && find_item("dst"));
    reset(); add_item("src",1); add_item("dst",2);
    assert(sd_card_rename("src","dst") == ESP_ERR_INVALID_ARG && find_item("dst"));
    reset(); add_item("src",1); add_item("dst",1); add_item("dst.obd-bak",1);
    assert(sd_card_rename("src","dst") == ESP_ERR_INVALID_STATE && rename_calls==0);

    reset(); add_item("src",3); add_item("dst",1);
    assert(sd_card_rename("src","dst") == ESP_OK && rename_calls==2);
    assert(!find_item("src") && find_item("dst")->kind==3 && !find_item("dst.obd-bak"));
    reset(); add_item("src",1); add_item("dst",1); fail_rename_first=1;
    assert(sd_card_rename("src","dst") == ESP_FAIL && find_item("src") && find_item("dst"));
    reset(); add_item("src",3); add_item("dst",1); fail_rename_first=2;
    assert(sd_card_rename("src","dst") == ESP_FAIL && rename_calls==3);
    assert(find_item("src")->kind==3 && find_item("dst")->kind==1 && !find_item("dst.obd-bak"));
    reset(); add_item("src",3); add_item("dst",1); fail_rename_first=2; fail_rename_second=3;
    assert(sd_card_rename("src","dst") == ESP_FAIL);
    assert(find_item("src")->kind==3 && !find_item("dst") && find_item("dst.obd-bak")->kind==1);
    reset(); add_item("src",1); add_item("dst",1); fail_unlink=true;
    assert(sd_card_rename("src","dst") == ESP_OK && find_item("dst") && find_item("dst.obd-bak"));
    reset(); char long_dst[SD_PATH_MAX]; memset(long_dst,'x',sizeof(long_dst)-1); long_dst[sizeof(long_dst)-1]=0;
    add_item("src",3); add_item(long_dst,1);
    assert(sd_card_rename("src",long_dst) == ESP_ERR_INVALID_SIZE);
    assert(find_item("src")->kind==3 && find_item(long_dst)->kind==1 && rename_calls==0);
    return 0;
}
