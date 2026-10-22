#pragma once
/*
 * Built-in fallback profile "obd2.generic" (the only profile compiled into
 * the firmware). vp_builtin_generic.c is generated from
 * vehicles/catalog/obd2.generic.vehicle.json by tools/vehicles/gen_builtin.py
 * (--check verifies it is current); do not edit it by hand.
 *
 * The blob is a compiled vp_profile_t (vp_profile_check() passes on it), so
 * the rollback path to generic needs neither cJSON nor the heap. The
 * minified JSON is kept for export.
 */

#include <stddef.h>
#include <stdint.h>

#include "vp_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compiled vp_profile_t blob (flash .rodata, 4-byte aligned). */
extern const uint32_t     vp_builtin_generic_blob[];
extern const size_t       vp_builtin_generic_blob_size;
/* Minified source JSON (NUL-terminated), for export. */
extern const char *const  vp_builtin_generic_json;
extern const size_t       vp_builtin_generic_json_len;

static inline const vp_profile_t *vp_builtin_generic(void)
{
    return (const vp_profile_t *)(const void *)vp_builtin_generic_blob;
}

#ifdef __cplusplus
}
#endif
