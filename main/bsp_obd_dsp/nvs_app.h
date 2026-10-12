#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* --------------------------------------------------------------------------
 * nvs_app — Application-private NVS partition (label "nvs_app", 0x11000, 60KB)
 *
 * The web flasher's complete-mode write path never touches this partition
 * (see docs/FLASH.md §3.2). It is the only storage area that survives a
 * web-flasher full reflash unchanged.
 *
 * Namespaces:
 *   sys   magic ('NAP2'), layout_crc, mig_ver
 *   imp   inbox CRC ledger (theme_0 / bootmedia last-seen CRC)
 *   veh   current vehicle profile id, prev id, installation state
 *
 * Call nvs_app_init() once at boot, before nvs_storage_init() and before
 * any first-boot migration logic that may read mig_ver.
 * -------------------------------------------------------------------------- */

/** CRC32 of all 9 partitions.csv entries (layout v2). */
#define NVS_APP_LAYOUT_CRC   0x94752aa3u

/** mig_ver value written after the first-boot partition-table migration. */
#define NVS_APP_MIG_VER_DONE 1u

/** Maximum vehicle profile id string length (including NUL terminator). */
#define NVS_APP_VEH_ID_LEN   64u

/**
 * @brief  Initialise the nvs_app partition.
 *
 * Calls nvs_flash_init_partition().  If the partition is corrupt it is erased
 * and re-initialised.  If the stored magic does not match 'NAP2' the partition
 * is erased and a fresh magic + layout_crc are written.
 *
 * Idempotent: safe to call again after a crash during migration.
 */
esp_err_t nvs_app_init(void);

/* ---- sys namespace ---- */

/** Current migration version (0 = not yet migrated). */
uint8_t   nvs_app_mig_ver_get(void);
esp_err_t nvs_app_mig_ver_set(uint8_t ver);

/* ---- imp namespace: inbox CRC ledger ---- */

/**
 * CRC32 of the theme_0 payload last successfully imported into SD / config.
 * 0 = no previous import.
 */
uint32_t  nvs_app_imp_theme_crc_get(void);
esp_err_t nvs_app_imp_theme_crc_set(uint32_t crc);

/**
 * CRC32 of the bootmedia payload last successfully imported.
 * 0 = no previous import.
 */
uint32_t  nvs_app_imp_bootmedia_crc_get(void);
esp_err_t nvs_app_imp_bootmedia_crc_set(uint32_t crc);

/* ---- veh namespace: current vehicle profile ---- */

/**
 * Vehicle profile installation state.
 * Stored as a uint8_t in key "state" of the veh namespace.
 * INSTALLED → PENDING → ACTIVE; failure path leads to FAILED.
 */
typedef enum {
    VEH_STATE_NONE      = 0, /* no profile beyond obd2.generic */
    VEH_STATE_INSTALLED = 1, /* profile stored in config partition */
    VEH_STATE_PENDING   = 2, /* installed, awaiting activation/validation */
    VEH_STATE_ACTIVE    = 3, /* running and validated */
    VEH_STATE_FAILED    = 4, /* validation failed; rolled back */
} veh_state_t;

/**
 * Get / set the currently active vehicle profile id string.
 * On set, the previous id is automatically copied to the "prev" key so
 * roll-back is possible.  Empty string means obd2.generic (built-in).
 */
esp_err_t nvs_app_veh_id_get(char *out, size_t out_len);
esp_err_t nvs_app_veh_id_set(const char *id);

/** Previously active profile id (the one to roll back to on FAILED). */
esp_err_t nvs_app_veh_prev_id_get(char *out, size_t out_len);

veh_state_t nvs_app_veh_state_get(void);
esp_err_t   nvs_app_veh_state_set(veh_state_t state);
