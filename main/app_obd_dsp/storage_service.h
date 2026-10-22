#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * storage_service - boot-time storage bring-up and the background SD task.
 *
 * app_main order:
 *   nvs_app_init(); nvs_storage_init(); signal_store_init();
 *   sd_card_init();                   (no I2C/EXIO needed: D3 has a pull-up)
 *   storage_service_boot();           migration / config mount / theme resume
 *   coredump_store_boot_check();
 *   vp_manager_init();
 *   ... theme_engine_init() / UI ...
 *   storage_service_start();          replaces app_main's sd_poll task
 * -------------------------------------------------------------------------- */

/* Synchronous, on the main task (internal stack), after sd_card_init() and
 * before vp_manager_init() / theme_engine_init():
 *   - first-boot layout migration when needed, else mount config (format +
 *     restore from the SD mirror only if it does not mount)
 *   - theme_library_resume(): an interrupted SD theme selection is resumed
 *     after a reboot
 *   - sd_import_scan_boot(): RESET.TXT / ACTIVATE.TXT / *.vehicle.json
 *   - pending legacy vehicle retry
 * Never fatal; returns the migration / config error for logging. */
esp_err_t storage_service_boot(void);

/* Start the background task (internal 8 KB stack, low priority): calls
 * sd_card_poll() at the card state's probe cadence (1 s mounted, 3 s absent/
 * ejected, 10 s unusable card) and, a few seconds after start and on every
 * card insert (also when the WiFi OTA server stops), runs theme resume,
 * legacy vehicle retry, inbox import, config mirror to SD and the full
 * /OBD/IMPORT scan. Card jobs are paused while the OTA server is active.
 * Without a configured SD slot, returns ESP_OK without creating a task.
 * A configured but empty slot still starts polling for insertion. This does
 * not skip storage_service_boot() or change storage_service_busy(). */
esp_err_t storage_service_start(void);

/* A storage job (import, scan, or SD theme write) is running: the UI should not
 * eject the card now. */
bool storage_service_busy(void);

#ifdef __cplusplus
}
#endif
