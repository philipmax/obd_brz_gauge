#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "vehicle/vp_profile.h"
#include "vehicle/vp_validate.h"
#include "bsp_obd_dsp/nvs_app.h"
#include "app_obd_dsp/signal_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * vp_manager - vehicle profile lifecycle (REFACTOR_PLAN.md P3).
 *
 * Storage: installed profiles are kept as source JSON in the config partition
 * (/config/vehicles/<id>.json, mirrored to SD /OBD/CFG); the active one is also
 * copied to nvs_app veh/blob so it survives a config wipe. The builtin
 * obd2.generic profile is always available and never stored.
 *
 * Lifecycle: install -> INSTALLED; activate(id) -> PENDING (takes effect on the
 * next boot); verification passes -> ACTIVE; 3 boots without passing -> roll
 * back to the previous profile, then to obd2.generic. Repeated crashes enter
 * safe mode (obd2.generic, no imports).
 *
 * Thread safety: the active profile pointer is stable for the whole boot (it
 * only changes across reboots), so readers need no locking.
 * -------------------------------------------------------------------------- */

#define VPM_GENERIC_ID   "obd2.generic"
#define VPM_ID_LEN       32   /* vp_profile_t.id */
#define VPM_NAME_LEN     24   /* vp_profile_t.name */

typedef struct {
    char        id[VPM_ID_LEN];
    char        name[VPM_NAME_LEN];
    uint16_t    rev;
    bool        builtin;   /* obd2.generic */
    bool        active;    /* running this boot */
    bool        pending;   /* will be tried on next boot */
    veh_state_t state;     /* state of this entry (INSTALLED until first activated) */
} vpm_entry_t;

/* Load the active profile. Call after nvs_app_init(), sd_card_init() and
 * storage_service_boot() (config mount + first-boot migration), before the
 * ELM327 client starts. Never fails: falls back to obd2.generic. install/activate/
 * reset/legacy_id work before init (they only touch storage + nvs_app). */
void vp_manager_init(void);

/* Active profile; never NULL. */
const vp_profile_t *vp_manager_active(void);
const char         *vp_manager_active_id(void);
veh_state_t         vp_manager_active_state(void);
bool                vp_manager_safe_mode(void);
/* Human-readable reason of the last failure (install/activate/rollback), "" if none. */
const char         *vp_manager_last_error(void);

/* Capability queries for UI gating. slot = signal_store slot (SIG_*). */
bool    vp_manager_has_signal(int slot);
uint8_t vp_manager_gear_count(void);   /* 0 when the profile has no drivetrain */

/* Validate + compile + store. issues may be NULL. out_id receives the profile id.
 * Does not activate. ESP_ERR_INVALID_ARG on validation errors (see issues). */
esp_err_t vp_manager_install_json(const char *json, size_t len, vp_issues_t *issues,
                                  char *out_id, size_t out_id_len);
/* Mark id PENDING for the next boot ("obd2.generic" = reset to generic). */
esp_err_t vp_manager_activate(const char *id);
esp_err_t vp_manager_reset_to_generic(void);
/* Switch back to the previous profile (PENDING for next boot). */
esp_err_t vp_manager_rollback(void);
/* Delete an installed profile (not the active one, not builtin). */
esp_err_t vp_manager_remove(const char *id);
/* Explicit bulk clear, including the current source and SD mirror. Requires
 * mounted SD; schedules builtin generic for reboot, retaining this boot's
 * compiled pointer. Catalog/import/export templates and media are untouched. */
esp_err_t vp_manager_clear_installed(void);
/* Fill up to max entries (builtin first); returns the count. */
int       vp_manager_list(vpm_entry_t *out, int max);
/* Copy the source JSON of id to SD /OBD/EXPORT/<id>.vehicle.json. */
esp_err_t vp_manager_export(const char *id);
/* Read the stored source JSON of id into a fresh PSRAM buffer (caller frees). */
esp_err_t vp_manager_read_json(const char *id, char **out, size_t *out_len);

/* Old nvs_user_cfg_t.vehicle_profile_idx (compile-time table order) -> catalog
 * id; NULL for 0 (generic) or out of range. Used by the first-boot migration. */
const char *vp_manager_legacy_id(uint8_t idx);

/* Verification hooks, called by the ELM327 client / vp_engine (single task). */
void vp_manager_on_elm_link(bool connected);
void vp_manager_on_voltage(float volts);       /* ATRV reading */
void vp_manager_on_response(int slot);         /* a positive response decoded into slot */

#ifdef __cplusplus
}
#endif
