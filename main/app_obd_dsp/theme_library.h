#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_obd_dsp/storage_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SD library /OBD/THEMES/<key>/theme.bin. Activation validates the file
 * and commits its key to NVS; no media is copied into Flash. Before activation
 * delete theme pages and call theme_unload(). Poll DONE / ERROR, reload with
 * theme_load(0), then ack. Missing SD resources use the built-in default UI. */

typedef enum {
    THEME_LIB_IDLE = 0,
    THEME_LIB_WRITING,   /* validating/selecting an SD package */
    THEME_LIB_DONE,      /* selection committed: theme_load(0), then ack */
    THEME_LIB_ERROR,     /* see theme_library_last_error(); ack to clear */
} theme_lib_state_t;

/* Start asynchronous SD validation and selection; refused during OTA or another storage job. */
esp_err_t theme_library_activate(const char *key);
/* Synchronous validation + NVS commit for OTA and migration workers. */
esp_err_t theme_library_select(const char *key);

/* Cheap; callable from any task. pct (optional) = progress 0..100. */
theme_lib_state_t theme_library_get_state(uint8_t *pct);
const char       *theme_library_last_error(void);
/* DONE / ERROR -> IDLE. */
void              theme_library_ack(void);

/* Persisted SD selection ("" means the built-in default). */
void theme_library_current_key(char *out, size_t out_len);

/* Recover a legacy interrupted activation by selecting its SD file. No Flash write. */
esp_err_t theme_library_resume(void);
bool      theme_library_resume_pending(void);

/* Move a dropped theme.bin (e.g. /OBD/IMPORT/x.bin) into
 * /OBD/THEMES/<key>/theme.bin (key = storage_theme_key(theme.id)), replacing
 * an older copy. Does not activate. key_out optional. */
esp_err_t theme_library_import_file(const char *path, char *key_out, size_t key_len);

/* Delete the selected theme directory and clear its persisted selection.
 * `deleted` is true when a directory was removed; an already-clear or stale
 * selection is still cleared successfully and reports false. */
esp_err_t theme_library_delete_current(bool *deleted);

#ifdef __cplusplus
}
#endif
