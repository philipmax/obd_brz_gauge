#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * first_boot_migration - partition layout v1 -> v2, once after the web
 * flasher complete reflash (REFACTOR_PLAN §3.5). Called by
 * storage_service_boot() after sd_card_init(), before vp_manager_init().
 *
 *   1. oversized old boot animation -> SD (inbox_import_bootmedia_rescue)
 *   2. old theme_0 / bootmedia -> SD library (inbox_import_run)
 *   3. erase coredump (held old bootmedia bytes)
 *   4. mount config; format + restore from the SD mirror if it does not mount
 *   5. vehicle: nvs_user_cfg_t.vehicle_profile_idx -> catalog id; install
 *      /OBD/CATALOG/<id>.vehicle.json and activate it. Without the file the
 *      id is remembered (nvs_app imp/mig_veh) and retried on card insert;
 *      the gauge runs obd2.generic meanwhile. The idx byte is left alone.
 *   6. layout_crc, then mig_ver = NVS_APP_MIG_VER_DONE (last)
 *
 * Re-entrant: every step is idempotent, so a power loss just reruns it.
 * Failing steps are logged and skipped (nothing here may brick the boot).
 * -------------------------------------------------------------------------- */

/* mig_ver < NVS_APP_MIG_VER_DONE or layout_crc != NVS_APP_LAYOUT_CRC. */
bool      first_boot_migration_needed(void);

/* Run the migration if needed (ESP_OK when nothing to do). Blocks for a few
 * seconds on the first boot (flash -> SD copies). */
esp_err_t first_boot_migration_run(void);

/* Mount the config LittleFS. If it does not mount, rescue an oversized boot
 * animation first (its tail may be what broke config), then format, mount
 * and restore from the SD mirror. Also used on regular boots. */
esp_err_t first_boot_migration_mount_config(void);

/* Pending legacy vehicle (imp/mig_veh): install + activate it once its
 * catalog JSON is on the card (or it was installed via /OBD/IMPORT), unless
 * another vehicle was chosen meanwhile. ESP_OK: nothing pending or done.
 * ESP_ERR_INVALID_STATE: still waiting (no card / file). */
esp_err_t first_boot_migration_retry_vehicle(void);

/* Forget the pending legacy vehicle (explicit reset to obd2.generic: a reset
 * on a gauge that already runs generic leaves no pending id behind). */
void      first_boot_migration_drop_vehicle(void);

#ifdef __cplusplus
}
#endif
