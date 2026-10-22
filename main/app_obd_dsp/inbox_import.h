#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * inbox_import - migrate legacy Flash media to SD. CRC ledger lives in
 * nvs_app imp/. New runtime media is never installed into Flash.
 *
 * theme_0: valid unseen payload -> copy to /OBD/THEMES/<key>/theme.bin;
 *          select only when no theme is selected. Preserve existing SD files.
 *          Empty legacy Flash does not change the selection or ledger.
 * bootmedia: valid unseen image -> SD_BOOT_LEGACY, preserving existing SD files.
 * Without a card migration is deferred until insertion.
 * Reads several MB of flash: call from storage_service, not the LVGL task.
 * -------------------------------------------------------------------------- */

/* ESP_OK: nothing new or imported. ESP_ERR_INVALID_STATE: deferred (no SD,
 * OTA active, another storage job running) - call again later. */
esp_err_t inbox_import_run(void);

/* Oversized legacy boot animation: an image at the bootmedia address that is
 * longer than the bootmedia partition (old 0x5E0000 slot, or an oversized
 * web-flasher image) has its tail in coredump/config, which are erased or
 * reformatted next. Call BEFORE config_part_format()/coredump erase: copies
 * the whole image to SD_BOOT_LEGACY, then erases the flash manifest (also
 * when the copy failed / there is no card: the new partition can never play
 * it). ESP_OK: nothing oversized, or rescued. ESP_ERR_INVALID_STATE:
 * oversized but no SD card (the animation is lost). Other: copy failed.
 * *hit_coredump (optional) = the image overwrote the coredump partition
 * (caller erases it). */
esp_err_t inbox_import_bootmedia_rescue(bool *hit_coredump);

#ifdef __cplusplus
}
#endif
