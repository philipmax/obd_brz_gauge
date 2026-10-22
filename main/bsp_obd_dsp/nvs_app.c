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
#define KEY_CRASH     "crash_cnt"
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
#define KEY_VEH_PEND  "pending"
#define KEY_VEH_BLOB  "blob"
#define KEY_VEH_BFAIL "boot_fail"

static bool s_ready;

/* -------------------------------------------------------------------------- */
/* Low-level helpers (open/close per call; the NVS API itself is thread-safe) */
/* -------------------------------------------------------------------------- */

static esp_err_t open_ns(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!ns || !ns[0]) return ESP_ERR_INVALID_ARG;
    return nvs_open_from_partition(PARTITION, ns, mode, h);
}

static uint8_t get_u8(const char *ns, const char *key, uint8_t def)
{
    nvs_handle_t h;
    uint8_t v = def;
    if (open_ns(ns, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, key, &v) != ESP_OK) v = def;
        nvs_close(h);
    }
    return v;
}

static esp_err_t set_u8(const char *ns, const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* String getter: missing key -> "" + ESP_OK (ids default to "none"). */
static esp_err_t get_str_or_empty(const char *ns, const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    esp_err_t err = nvs_app_get_str(ns, key, out, out_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

/* -------------------------------------------------------------------------- */

static esp_err_t s_sys_write_magic(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PARTITION, NS_SYS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    /* layout_crc is deliberately NOT written here: it is recorded by the
     * first-boot migration once it has finished (0 = never migrated). */
    err = nvs_set_u32(h, KEY_MAGIC, MAGIC_VAL);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_MIG_VER, 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t erase_and_init(void)
{
    esp_err_t err = nvs_flash_erase_partition(PARTITION);
    if (err == ESP_OK) err = nvs_flash_init_partition(PARTITION);
    return err;
}

esp_err_t nvs_app_init(void)
{
    if (s_ready) return ESP_OK;

    esp_err_t err = nvs_flash_init_partition(PARTITION);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "partition '%s' missing (old partition table?)", PARTITION);
        return err;
    }
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        /* First boot after the layout change this range held phy_init data
         * and free space; any "not a valid NVS" result means start fresh. */
        ESP_LOGW(TAG, "init: %s, erasing", esp_err_to_name(err));
        err = erase_and_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Check magic. Only a definite "absent/different" erases; a transient
     * error never wipes the vehicle profile backup. */
    nvs_handle_t h;
    uint32_t magic = 0;
    esp_err_t merr = nvs_open_from_partition(PARTITION, NS_SYS, NVS_READONLY, &h);
    if (merr == ESP_OK) {
        merr = nvs_get_u32(h, KEY_MAGIC, &magic);
        nvs_close(h);
    }
    bool fresh = (merr == ESP_ERR_NVS_NOT_FOUND) || (merr == ESP_OK && magic != MAGIC_VAL);
    if (fresh) {
        ESP_LOGI(TAG, "magic mismatch (0x%08x), writing fresh", (unsigned)magic);
        err = erase_and_init();
        if (err == ESP_OK) err = s_sys_write_magic();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write magic failed: %s", esp_err_to_name(err));
            return err;
        }
    } else if (merr != ESP_OK) {
        ESP_LOGW(TAG, "magic read: %s (keeping data)", esp_err_to_name(merr));
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready, mig_ver=%u layout=0x%08x", nvs_app_mig_ver_get(),
             (unsigned)nvs_app_layout_crc_get());
    return ESP_OK;
}

/* ---- sys ---- */

uint8_t   nvs_app_mig_ver_get(void)          { return get_u8(NS_SYS, KEY_MIG_VER, 0); }
esp_err_t nvs_app_mig_ver_set(uint8_t ver)   { return set_u8(NS_SYS, KEY_MIG_VER, ver); }

uint32_t  nvs_app_layout_crc_get(void)       { return nvs_app_get_u32(NS_SYS, KEY_LAYOUT, 0); }
esp_err_t nvs_app_layout_crc_set(uint32_t c) { return nvs_app_set_u32(NS_SYS, KEY_LAYOUT, c); }

uint8_t   nvs_app_crash_count_get(void)      { return get_u8(NS_SYS, KEY_CRASH, 0); }
esp_err_t nvs_app_crash_count_set(uint8_t n) { return set_u8(NS_SYS, KEY_CRASH, n); }

/* ---- imp ---- */

uint32_t  nvs_app_imp_theme_crc_get(void)         { return nvs_app_get_u32(NS_IMP, KEY_THEME_CRC, 0); }
esp_err_t nvs_app_imp_theme_crc_set(uint32_t crc) { return nvs_app_set_u32(NS_IMP, KEY_THEME_CRC, crc); }

uint32_t  nvs_app_imp_bootmedia_crc_get(void)         { return nvs_app_get_u32(NS_IMP, KEY_BM_CRC, 0); }
esp_err_t nvs_app_imp_bootmedia_crc_set(uint32_t crc) { return nvs_app_set_u32(NS_IMP, KEY_BM_CRC, crc); }

/* ---- veh ---- */

esp_err_t nvs_app_veh_id_get(char *out, size_t out_len)
{
    return get_str_or_empty(NS_VEH, KEY_VEH_ID, out, out_len);
}

esp_err_t nvs_app_veh_id_set(const char *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    if (strlen(id) >= NVS_APP_VEH_ID_LEN) return ESP_ERR_INVALID_SIZE;
    /* Save current -> prev before overwriting (both in one commit). */
    char prev[NVS_APP_VEH_ID_LEN];
    nvs_app_veh_id_get(prev, sizeof(prev));

    nvs_handle_t h;
    esp_err_t err = open_ns(NS_VEH, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (strcmp(prev, id) != 0) {
        err = nvs_set_str(h, KEY_VEH_PREV, prev);   /* "" = generic */
    }
    if (err == ESP_OK) err = nvs_set_str(h, KEY_VEH_ID, id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_app_veh_prev_id_get(char *out, size_t out_len)
{
    return get_str_or_empty(NS_VEH, KEY_VEH_PREV, out, out_len);
}

veh_state_t nvs_app_veh_state_get(void)
{
    uint8_t state = get_u8(NS_VEH, KEY_VEH_STATE, VEH_STATE_NONE);
    if (state > VEH_STATE_FAILED) state = VEH_STATE_NONE;
    return (veh_state_t)state;
}

esp_err_t nvs_app_veh_state_set(veh_state_t state)
{
    if ((unsigned)state > VEH_STATE_FAILED) return ESP_ERR_INVALID_ARG;
    return set_u8(NS_VEH, KEY_VEH_STATE, (uint8_t)state);
}

esp_err_t nvs_app_veh_pending_id_get(char *out, size_t out_len)
{
    return get_str_or_empty(NS_VEH, KEY_VEH_PEND, out, out_len);
}

esp_err_t nvs_app_veh_pending_id_set(const char *id)
{
    if (!id || !id[0]) return nvs_app_erase_key(NS_VEH, KEY_VEH_PEND);
    if (strlen(id) >= NVS_APP_VEH_ID_LEN) return ESP_ERR_INVALID_SIZE;
    return nvs_app_set_str(NS_VEH, KEY_VEH_PEND, id);
}

esp_err_t nvs_app_veh_blob_get(void *buf, size_t *len)
{
    return nvs_app_get_blob(NS_VEH, KEY_VEH_BLOB, buf, len);
}

esp_err_t nvs_app_veh_blob_set(const void *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > NVS_APP_VEH_BLOB_MAX) return ESP_ERR_INVALID_SIZE;
    return nvs_app_set_blob(NS_VEH, KEY_VEH_BLOB, buf, len);
}

esp_err_t nvs_app_veh_blob_erase(void)
{
    return nvs_app_erase_key(NS_VEH, KEY_VEH_BLOB);
}

uint8_t   nvs_app_veh_boot_fail_get(void)      { return get_u8(NS_VEH, KEY_VEH_BFAIL, 0); }
esp_err_t nvs_app_veh_boot_fail_set(uint8_t n) { return set_u8(NS_VEH, KEY_VEH_BFAIL, n); }

/* ---- generic access ---- */

uint32_t nvs_app_get_u32(const char *ns, const char *key, uint32_t def)
{
    nvs_handle_t h;
    uint32_t v = def;
    if (!key || open_ns(ns, NVS_READONLY, &h) != ESP_OK) return def;
    if (nvs_get_u32(h, key, &v) != ESP_OK) v = def;
    nvs_close(h);
    return v;
}

esp_err_t nvs_app_set_u32(const char *ns, const char *key, uint32_t val)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_app_get_str(const char *ns, const char *key, char *out, size_t out_len)
{
    if (!key || !out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NVS_NOT_FOUND : err;
    }
    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) out[0] = '\0';
    return err;
}

esp_err_t nvs_app_set_str(const char *ns, const char *key, const char *val)
{
    if (!key || !val) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_app_get_blob(const char *ns, const char *key, void *buf, size_t *len)
{
    if (!key || !len) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, key, buf, len);   /* buf == NULL -> size query */
    nvs_close(h);
    return err;
}

esp_err_t nvs_app_set_blob(const char *ns, const char *key, const void *buf, size_t len)
{
    if (!key || (!buf && len)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_app_erase_key(const char *ns, const char *key)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
