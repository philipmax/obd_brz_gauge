/* Real legacy partition lookup/import; storage/NVS/SD boundaries are simulated. */
#include <stdbool.h>
#include <string.h>
#include "th_util.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "app_obd_dsp/inbox_import.h"
#include "app_obd_dsp/theme_mount.h"
#include "app_obd_dsp/theme_library.h"
#include "bsp_obd_dsp/nvs_app.h"
#include "bsp_obd_dsp/sd_card.h"

static const esp_partition_t theme = {.address=0x620000, .size=0x400000,
    .type=ESP_PARTITION_TYPE_DATA, .subtype=ESP_PARTITION_SUBTYPE_DATA_SPIFFS, .label="theme_0"};
static const esp_partition_t boot = {.address=0xa20000, .size=0x4c0000,
    .type=ESP_PARTITION_TYPE_DATA, .subtype=0x82, .label="bootmedia"};
static const esp_partition_t core = {.address=0xee0000, .size=0x20000,
    .type=ESP_PARTITION_TYPE_DATA, .subtype=ESP_PARTITION_SUBTYPE_DATA_COREDUMP, .label="coredump"};
static bool mounted, theme_exists, boot_exists, ota, busy, pending;
static esp_err_t manifest_error, selection_error;
static uint32_t theme_ledger, boot_ledger, boot_len;
static unsigned copies, selects, invalidates, depth;
static char selection[65];
static const uint32_t theme_crc=0x12345678u, boot_crc=0x23456789u;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                               esp_partition_subtype_t subtype,
                                               const char *label)
{
    const esp_partition_t *parts[]={&theme,&boot,&core};
    for (unsigned i=0;i<3;i++) {
        const esp_partition_t *p=parts[i];
        if (p->type==type && (subtype==ESP_PARTITION_SUBTYPE_ANY || subtype==p->subtype) &&
            (!label || !strcmp(label,p->label))) return p;
    }
    return NULL;
}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t off,void *dst,size_t len)
{ TH_CHECK(off+len<=p->size); memset(dst,0x55,len); return ESP_OK; }
esp_err_t esp_flash_get_size(void *chip,uint32_t *size)
{ (void)chip; *size=0x1000000; return ESP_OK; }
bool sd_card_is_mounted(void) { return mounted; }
bool ota_wifi_server_is_active(void) { return ota; }
bool storage_job_lock(TickType_t wait) { (void)wait; depth++; return true; }
void storage_job_unlock(void) { TH_CHECK(depth>0); depth--; }
bool theme_library_resume_pending(void) { return pending; }
theme_lib_state_t theme_library_get_state(uint8_t *percent)
{ (void)percent; return busy ? THEME_LIB_WRITING : THEME_LIB_IDLE; }
void theme_library_current_key(char *out,size_t len) { snprintf(out,len,"%s",selection); }
esp_err_t theme_library_select(const char *key)
{ selects++; if (selection_error) return selection_error; snprintf(selection,sizeof(selection),"%s",key); return ESP_OK; }
uint32_t nvs_app_imp_theme_crc_get(void) { return theme_ledger; }
esp_err_t nvs_app_imp_theme_crc_set(uint32_t crc) { theme_ledger=crc; return ESP_OK; }
uint32_t nvs_app_imp_bootmedia_crc_get(void) { return boot_ledger; }
esp_err_t nvs_app_imp_bootmedia_crc_set(uint32_t crc) { boot_ledger=crc; return ESP_OK; }
esp_err_t storage_theme_meta_from_flash(storage_theme_meta_t *out)
{
    if (manifest_error) return manifest_error;
    memset(out,0,sizeof(*out)); strcpy(out->id,"legacy"); out->used_len=16386; return ESP_OK;
}
void storage_theme_key(const char *id,char *out,size_t len) { snprintf(out,len,"%s",id); }
esp_err_t storage_flash_crc32(uint32_t addr,uint32_t len,uint32_t *crc)
{ TH_CHECK(addr==theme.address && len==16386); *crc=theme_crc; return ESP_OK; }
int64_t storage_sd_file_size(const char *path)
{ return strstr(path,"theme.bin") ? (theme_exists ? 16386 : -1) : (boot_exists ? 8192 : -1); }
esp_err_t storage_copy_flash_to_sd(uint32_t addr,uint32_t len,const char *dst,uint32_t *crc)
{
    (void)len; TH_CHECK(depth>0); copies++;
    if (addr==theme.address) { TH_CHECK(strstr(dst,"/legacy/theme.bin")!=NULL); theme_exists=true; }
    else { TH_CHECK(addr==boot.address && !strcmp(dst,SD_BOOT_LEGACY)); boot_exists=true; }
    if (crc) *crc=theme_crc; return ESP_OK;
}
esp_err_t storage_bootmedia_probe(uint32_t addr,uint32_t avail,uint32_t *len,uint32_t *crc)
{
    TH_CHECK(addr==boot.address); if (!boot_len) return ESP_ERR_NOT_FOUND;
    TH_CHECK(boot_len<=avail); *len=boot_len; *crc=boot_crc; return ESP_OK;
}
bool boot_media_invalidate_manifest(void) { invalidates++; return true; }
static void reset(void)
{
    mounted=true; theme_exists=boot_exists=ota=busy=pending=false;
    manifest_error=selection_error=ESP_OK;
    theme_ledger=boot_ledger=boot_len=0; copies=selects=invalidates=depth=0; selection[0]=0;
}
int main(void)
{
    reset(); TH_CHECK(theme_mount()); // subtype ANY must find actual SPIFFS theme_0
    TH_CHECK(theme_get_partition_addr()==theme.address && theme_get_partition_size()==theme.size);
    uint8_t byte; TH_CHECK(theme_raw_read(0,&byte,1)==ESP_OK && byte==0x55);
    TH_CHECK(theme_raw_read(theme.size,&byte,1)==ESP_ERR_INVALID_SIZE);
    TH_CHECK(inbox_import_run()==ESP_OK && copies==1 && selects==1 && !strcmp(selection,"legacy"));
    TH_CHECK(theme_ledger==theme_crc && depth==0);
    TH_CHECK(inbox_import_run()==ESP_OK && copies==1 && selects==1); // ledger prevents re-import
    reset(); strcpy(selection,"new"); theme_exists=true;
    TH_CHECK(inbox_import_run()==ESP_OK && !copies && !selects && !strcmp(selection,"new"));
    reset(); manifest_error=ESP_ERR_NOT_FOUND; strcpy(selection,"new"); theme_ledger=91;
    TH_CHECK(inbox_import_run()==ESP_OK && !strcmp(selection,"new") && theme_ledger==91);
    reset(); mounted=false;
    TH_CHECK(inbox_import_run()==ESP_ERR_INVALID_STATE && !copies && !selects && depth==0);
    mounted=true; TH_CHECK(inbox_import_run()==ESP_OK && copies==1 && selects==1);
    reset(); selection_error=ESP_FAIL;
    TH_CHECK(inbox_import_run()==ESP_FAIL && copies==1 && !theme_ledger && depth==0);
    selection_error=ESP_OK;
    TH_CHECK(inbox_import_run()==ESP_OK && copies==1 && selects==2 && theme_ledger==theme_crc);
    reset(); manifest_error=ESP_ERR_NOT_FOUND; boot_len=8192; boot_exists=true;
    TH_CHECK(inbox_import_run()==ESP_OK && !copies && boot_ledger==boot_crc);
    reset(); manifest_error=ESP_ERR_NOT_FOUND; boot_len=8192;
    TH_CHECK(inbox_import_run()==ESP_OK && copies==1 && boot_exists);
    reset(); ota=true; TH_CHECK(inbox_import_run()==ESP_ERR_INVALID_STATE && !depth && !copies);
    reset(); busy=true; TH_CHECK(inbox_import_run()==ESP_OK && !copies);
    reset(); pending=true; TH_CHECK(inbox_import_run()==ESP_OK && !copies);
    reset(); boot_len=boot.size+256; bool hit=false;
    TH_CHECK(inbox_import_bootmedia_rescue(&hit)==ESP_OK && hit && copies==1 && invalidates==1 && !depth);
    reset(); boot_len=boot.size+256; boot_exists=true;
    TH_CHECK(inbox_import_bootmedia_rescue(NULL)==ESP_OK && !copies && invalidates==1 && !depth);
    reset(); boot_len=boot.size+256; mounted=false;
    TH_CHECK(inbox_import_bootmedia_rescue(NULL)==ESP_ERR_INVALID_STATE && !copies && invalidates==1);
    return th_report("test_media_migration");
}
