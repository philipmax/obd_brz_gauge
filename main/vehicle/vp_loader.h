#pragma once
/**
 * vp_loader.h - Vehicle profile loader and runtime accessor
 *
 * Provides the global active profile pointer and loading functions.
 * This replaces the old vehicle_profile_get_active() API.
 */

#include "vp_profile.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the loader with the built-in generic profile
 */
void vp_loader_init(void);

/**
 * Get the currently active vehicle profile (never NULL after init)
 */
const vp_profile_t* vp_loader_get_active(void);

/**
 * Load a profile by index (0 = built-in generic)
 * Returns true on success
 */
bool vp_loader_load_by_index(uint8_t index);

/**
 * Get profile count (built-in + loaded from storage)
 */
uint8_t vp_loader_get_count(void);

/**
 * Get profile name by index (for UI selection)
 */
const char* vp_loader_get_name_by_index(uint8_t index);

#ifdef __cplusplus
}
#endif
