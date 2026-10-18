#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * sd_import - processes files dropped into /OBD/IMPORT (REFACTOR_PLAN P3).
 *
 *   *.vehicle.json    validate + install (vp_manager_install_json); NOT activated
 *   *.gaugeproj.json  theme sidecar install (theme_sidecar_install_file)
 *   *.bin             theme.bin moved to /OBD/THEMES/<key>/theme.bin (not activated)
 *   ACTIVATE.TXT      first line = vehicle profile id -> active from next boot
 *   RESET.TXT         back to obd2.generic (processed last, wins over ACTIVATE.TXT)
 *
 * Each processed file is renamed to <name>.ok or <name>.err (+ <name>.err.txt
 * with the reasons); a moved *.bin leaves a small <name>.ok note. One CRLF
 * line per file is appended to /OBD/IMPORT/IMPORT.LOG. In vehicle safe mode
 * only RESET.TXT is processed. Stale *.part files are removed.
 *
 * Calls are serialized (storage job lock); run them on a worker task.
 * ESP_ERR_INVALID_STATE: no card mounted, WiFi OTA active (scanning is paused
 * in OTA mode), a theme library write running, or another job held the
 * storage lock for too long.
 * -------------------------------------------------------------------------- */

/* Full scan (storage_service on card insert / after boot, UI "SCAN"). */
esp_err_t sd_import_scan(void);

/* Boot pass, before vp_manager_init(): RESET.TXT, ACTIVATE.TXT and
 * *.vehicle.json only, so a reset / new profile takes effect this boot. */
esp_err_t sd_import_scan_boot(void);

/* Files that succeeded / failed in the last completed scan (either kind). */
void sd_import_last_summary(int *ok, int *err);

#ifdef __cplusplus
}
#endif
