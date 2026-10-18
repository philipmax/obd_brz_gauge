// first_boot_migration.c — partition layout v1 -> v2 after the web-flasher
// complete reflash (REFACTOR_PLAN §3.5). Steps and contract: see the header.
#include "app_obd_dsp/first_boot_migration.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp_obd_dsp/config_part.h"
#include "bsp_obd_dsp/nvs_app.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/coredump_store.h"
#include "app_obd_dsp/inbox_import.h"
#include "app_obd_dsp/storage_util.h"
#include "vehicle/vp_manager.h"
#include "vehicle/vp_validate.h"

static const char *TAG = "migration";

#define NS_SYS        "sys"
#define KEY_MIG_VEH   "mig_veh"   /* str: legacy vehicle id waiting for its catalog JSON */
#define PATH_LEN      (sizeof(SD_DIR_CATALOG) + VPM_ID_LEN + 16)

bool first_boot_migration_needed(void)
{
    return nvs_app_mig_ver_get() < NVS_APP_MIG_VER_DONE ||
           nvs_app_layout_crc_get() != NVS_APP_LAYOUT_CRC;
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

esp_err_t first_boot_migration_mount_config(void)
{
    esp_err_t err = config_part_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) return ESP_OK;
    ESP_LOGW(TAG, "config does not mount (%s): rescue, format, restore", esp_err_to_name(err));

    /* An oversized boot animation (old layout / old flasher) may be what
     * overwrote config: save it before its tail is gone. */
    bool hit_coredump = false;
    inbox_import_bootmedia_rescue(&hit_coredump);
    if (hit_coredump) coredump_store_erase();

    err = config_part_format();
    if (err == ESP_OK) err = config_part_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config unusable: %s", esp_err_to_name(err));
        return err;
    }
    if (sd_card_is_mounted()) {
        esp_err_t r = config_part_restore_from_sd();
        if (r != ESP_OK) ESP_LOGW(TAG, "config restore from SD: %s", esp_err_to_name(r));
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* vehicle                                                             */
/* ------------------------------------------------------------------ */

/* The user picked a vehicle on the new firmware (or reset to generic). */
static bool vehicle_chosen(void)
{
    char id[NVS_APP_VEH_ID_LEN];
    if (nvs_app_veh_pending_id_get(id, sizeof(id)) == ESP_OK && id[0]) return true;
    return nvs_app_veh_id_get(id, sizeof(id)) == ESP_OK && id[0] && strcmp(id, VPM_GENERIC_ID) != 0;
}

/* Install /OBD/CATALOG/<id>.vehicle.json (or <id>.json) if present, then
 * activate id (also covers a profile installed earlier via /OBD/IMPORT). */
static esp_err_t install_legacy(const char *id)
{
    static const char *const fmts[] = {
        SD_DIR_CATALOG "/%s.vehicle.json",
        SD_DIR_CATALOG "/%s.json",
    };
    char path[PATH_LEN];
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; sd_card_is_mounted() && i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        snprintf(path, sizeof(path), fmts[i], id);
        if (!sd_card_exists(path)) continue;
        void *json = NULL;
        size_t len = 0;
        err = sd_card_read_file_alloc(path, VP_MAX_JSON_BYTES, &json, &len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "read %s: %s", path, esp_err_to_name(err));
            break;
        }
        char got[VPM_ID_LEN] = "";
        err = vp_manager_install_json((const char *)json, len, NULL, got, sizeof(got));
        heap_caps_free(json);
        if (err != ESP_OK) {
            const char *why = vp_manager_last_error();
            ESP_LOGW(TAG, "install %s: %s", path, (why && why[0]) ? why : esp_err_to_name(err));
            break;
        }
        if (strcmp(got, id) != 0) ESP_LOGW(TAG, "%s holds profile '%s', expected '%s'", path, got, id);
        ESP_LOGI(TAG, "installed legacy vehicle %s from %s", got, path);
        return vp_manager_activate(got);
    }
    /* no catalog file: maybe it is installed already */
    if (vp_manager_activate(id) == ESP_OK) return ESP_OK;
    return (err == ESP_ERR_NOT_FOUND) ? ESP_ERR_INVALID_STATE : err;
}

static void migrate_vehicle(void)
{
    uint8_t idx = nvs_cfg_read().vehicle_profile_idx;
    if (idx == 0) return;   /* obd2.generic */
    const char *id = vp_manager_legacy_id(idx);
    if (!id || !id[0]) {
        ESP_LOGW(TAG, "legacy vehicle index %u has no catalog id, using " VPM_GENERIC_ID, idx);
        return;
    }
    if (vehicle_chosen()) return;
    esp_err_t err = install_legacy(id);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "legacy vehicle %u -> %s", idx, id);
        nvs_app_erase_key(NS_SYS, KEY_MIG_VEH);
        return;
    }
    ESP_LOGW(TAG, "legacy vehicle %u -> %s: put %s/%s.vehicle.json on the card (%s); running "
             VPM_GENERIC_ID " meanwhile", idx, id, SD_DIR_CATALOG, id, esp_err_to_name(err));
    nvs_app_set_str(NS_SYS, KEY_MIG_VEH, id);
}

esp_err_t first_boot_migration_retry_vehicle(void)
{
    char id[VPM_ID_LEN];
    if (nvs_app_get_str(NS_SYS, KEY_MIG_VEH, id, sizeof(id)) != ESP_OK || !id[0]) return ESP_OK;
    if (vehicle_chosen()) {
        ESP_LOGI(TAG, "vehicle chosen meanwhile, dropping legacy %s", id);
        nvs_app_erase_key(NS_SYS, KEY_MIG_VEH);
        return ESP_OK;
    }
    if (vp_manager_safe_mode()) return ESP_ERR_INVALID_STATE;
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_INVALID_STATE;
    esp_err_t err = install_legacy(id);
    storage_job_unlock();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "legacy vehicle %s installed, active from the next boot", id);
        nvs_app_erase_key(NS_SYS, KEY_MIG_VEH);
    }
    return err == ESP_OK ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void first_boot_migration_drop_vehicle(void)
{
    nvs_app_erase_key(NS_SYS, KEY_MIG_VEH);
}

/* ------------------------------------------------------------------ */

esp_err_t first_boot_migration_run(void)
{
    if (!first_boot_migration_needed()) return ESP_OK;
    ESP_LOGW(TAG, "partition layout migration (mig_ver %u, layout %08lx), SD %s",
             nvs_app_mig_ver_get(), (unsigned long)nvs_app_layout_crc_get(),
             sd_card_is_mounted() ? "mounted" : "absent");
    storage_job_lock(portMAX_DELAY);

    /* 1. before anything erases coredump/config */
    bool hit_coredump = false;
    esp_err_t err = inbox_import_bootmedia_rescue(&hit_coredump);
    if (err != ESP_OK) ESP_LOGW(TAG, "bootmedia rescue: %s", esp_err_to_name(err));

    /* 2. old theme_0 / bootmedia that fit -> SD (deferred without a card) */
    err = inbox_import_run();
    if (err != ESP_OK) ESP_LOGW(TAG, "inbox import: %s", esp_err_to_name(err));

    /* 3. coredump held old bootmedia bytes */
    err = coredump_store_erase();
    if (err != ESP_OK) ESP_LOGW(TAG, "coredump erase: %s", esp_err_to_name(err));

    /* 4. config: old bootmedia bytes do not mount -> format + restore */
    err = first_boot_migration_mount_config();
    if (err != ESP_OK) ESP_LOGW(TAG, "config: %s", esp_err_to_name(err));

    /* 5. vehicle (the system-NVS cfg blob itself is left unchanged) */
    migrate_vehicle();

    /* 6. done */
    err = nvs_app_layout_crc_set(NVS_APP_LAYOUT_CRC);
    if (err == ESP_OK) err = nvs_app_mig_ver_set(NVS_APP_MIG_VER_DONE);
    storage_job_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot record migration (%s): it reruns next boot", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "migration done");
    return ESP_OK;
}
