#include "nvs_app.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define TAG           "nvs_app"
#define PARTITION     "nvs_app"

/* sys namespace */
#define NS_SYS        "sys"
#define KEY_MAGIC     "magic"
#define KEY_LAYOUT    "layout_crc"
#define KEY_MIG_VER   "mig_ver"
#define MAGIC_VAL     0x4E415032u  /* 'NAP2' little-endian */

/* imp namespace */
#define NS_IMP        "imp"
#define KEY_THEME_CRC "th_crc"
#define KEY_BM_CRC    "bm_crc"

/* veh namespace */
#define NS_VEH        "veh"
#define KEY_VEH_ID    "id"
#define KEY_VEH_PREV  "prev_id"
#define KEY_VEH_STATE "state"

/* -------------------------------------------------------------------------- */

static esp_err_t s_sys_write_magic(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PARTITION, NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, KEY_MAGIC, MAGIC_VAL);
    if (err == ESP_OK) err = nvs_set_u32(h, KEY_LAYOUT, NVS_APP_LAYOUT_CRC);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_MIG_VER, 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_app_init(void)
{
    /* Attempt to register and initialise the partition. */
    esp_err_t err = nvs_flash_init_partition(PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_app corrupt, erasing");
        nvs_flash_erase_partition(PARTITION);
        err = nvs_flash_init_partition(PARTITION);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Check magic; erase + rewrite on mismatch (first boot after reflash). */
    nvs_handle_t h;
    uint32_t magic = 0;
    if (nvs_open_from_partition(PARTITION, NS_SYS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, KEY_MAGIC, &magic);
        nvs_close(h);
    }
    if (magic != MAGIC_VAL) {
        ESP_LOGI(TAG, "magic mismatch (got 0x%08x), writing fresh", (unsigned)magic);
        nvs_flash_erase_partition(PARTITION);
        nvs_flash_init_partition(PARTITION);
        err = s_sys_write_magic();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write magic failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "ready, mig_ver=%u", nvs_app_mig_ver_get());
    return ESP_OK;
}

/* ---- sys ---- */

uint8_t nvs_app_mig_ver_get(void)
{
    nvs_handle_t h;
    uint8_t ver = 0;
    if (nvs_open_from_partition(PARTITION, NS_SYS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_MIG_VER, &ver);
        nvs_close(h);
    }
    return ver;
}

esp_err_t nvs_app_mig_ver_set(uint8_t ver)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PARTITION, NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, KEY_MIG_VER, ver);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---- imp ---- */

static uint32_t imp_crc_get(const char *key)
{
    nvs_handle_t h;
    uint32_t crc = 0;
    if (nvs_open_from_partition(PARTITION, NS_IMP, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, key, &crc);
        nvs_close(h);
    }
    return crc;
}

static esp_err_t imp_crc_set(const char *key, uint32_t crc)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PARTITION, NS_IMP, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, key, crc);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint32_t  nvs_app_imp_theme_crc_get(void)    { return imp_crc_get(KEY_THEME_CRC); }
esp_err_t nvs_app_imp_theme_crc_set(uint32_t crc) { return imp_crc_set(KEY_THEME_CRC, crc); }

uint32_t  nvs_app_imp_bootmedia_crc_get(void) { return imp_crc_get(KEY_BM_CRC); }
esp_err_t nvs_app_imp_bootmedia_crc_set(uint32_t crc) { return imp_crc_set(KEY_BM_CRC, crc); }

/* ---- veh ---- */

esp_err_t nvs_app_veh_id_get(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open_from_partition(PARTITION, NS_VEH, NVS_READONLY, &h) != ESP_OK)
        return ESP_ERR_NVS_NOT_FOUND;
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, KEY_VEH_ID, out, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { out[0] = '\0'; return ESP_OK; }
    return err;
}

esp_err_t nvs_app_veh_id_set(const char *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    /* Save current → prev before overwriting. */
    char prev[NVS_APP_VEH_ID_LEN];
    nvs_app_veh_id_get(prev, sizeof(prev));

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PARTITION, NS_VEH, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (prev[0] != '\0')
        nvs_set_str(h, KEY_VEH_PREV, prev);
    err = nvs_set_str(h, KEY_VEH_ID, id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_app_veh_prev_id_get(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open_from_partition(PARTITION, NS_VEH, NVS_READONLY, &h) != ESP_OK)
        return ESP_ERR_NVS_NOT_FOUND;
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, KEY_VEH_PREV, out, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { out[0] = '\0'; return ESP_OK; }
    return err;
}

veh_state_t nvs_app_veh_state_get(void)
{
    nvs_handle_t h;
    uint8_t state = VEH_STATE_NONE;
    if (nvs_open_from_partition(PARTITION, NS_VEH, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_VEH_STATE, &state);
        nvs_close(h);
    }
    if (state > VEH_STATE_FAILED) state = VEH_STATE_NONE;
    return (veh_state_t)state;
}

esp_err_t nvs_app_veh_state_set(veh_state_t state)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PARTITION, NS_VEH, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, KEY_VEH_STATE, (uint8_t)state);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
