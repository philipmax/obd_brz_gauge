#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* config partition — LittleFS (1MB, label "config")
 * Mounted at /config. Stores imported vehicle profiles, rule sidecars,
 * and index files. Never auto-formatted on mount failure: caller must
 * call config_part_format() explicitly.
 *
 * Quota: 75% of partition size (768KB). Writes are rejected at quota.
 * SD mirror: every write/delete is mirrored to SD /OBD/CFG when a card
 * is present. The mirror is best-effort; failure does not abort the write.
 */

#define CONFIG_MOUNT_POINT      "/config"
#define CONFIG_PART_LABEL       "config"
#define CONFIG_QUOTA_PCT        75      /* reject writes above this % full */

/* Initialise and mount the config LittleFS partition.
 * Returns ESP_OK on success, ESP_ERR_INVALID_STATE if already mounted,
 * or an error if the filesystem is corrupt (caller should call
 * config_part_format() then config_part_init() again). */
esp_err_t config_part_init(void);

/* Erase and reformat the config partition (destructive).
 * Only call this when init fails or on explicit user factory-reset. */
esp_err_t config_part_format(void);

/* Returns true if the partition is currently mounted. */
bool config_part_is_mounted(void);

/* Fill *used_bytes and *total_bytes with the current usage.
 * total_bytes is capped at the quota (CONFIG_QUOTA_PCT % of real size). */
esp_err_t config_part_get_usage(size_t *used_bytes, size_t *total_bytes);

/* Returns true if there is enough quota remaining for `needed_bytes`. */
bool config_part_has_quota(size_t needed_bytes);

/* Write data to a file inside the config partition.
 * Path must begin with CONFIG_MOUNT_POINT or a bare "/"-relative path.
 * Intermediate directories are created automatically.
 * Rejects the write if it would exceed the quota.
 * On success, mirrors the file to SD if a card is mounted. */
esp_err_t config_part_write(const char *path, const void *data, size_t len);

/* Read a file from the config partition into a caller-supplied buffer.
 * *bytes_read is set to the number of bytes actually read. */
esp_err_t config_part_read(const char *path, void *buf, size_t buf_len,
                            size_t *bytes_read);

/* Delete a file. Returns ESP_OK if the file did not exist. */
esp_err_t config_part_remove(const char *path);

/* Check whether a file exists. */
bool config_part_exists(const char *path);

/* Mirror the entire config partition to SD /OBD/CFG (best-effort).
 * Called on first mount after a successful SD card_init(), and after
 * any write/delete that succeeded. Failures are logged but not fatal. */
void config_part_mirror_to_sd(void);

/* Restore the config partition from the SD /OBD/CFG mirror.
 * Called during first-boot migration (REFACTOR_PLAN §3.5 step 5). */
esp_err_t config_part_restore_from_sd(void);

#ifdef __cplusplus
}
#endif
