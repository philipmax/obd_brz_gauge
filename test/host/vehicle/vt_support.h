#pragma once
/* Shared helpers for the vehicle host tests and tools. */

#include <stddef.h>

#include "vp_validate.h"
#include "vp_profile.h"

/*
 * Compiled-blob size limit of the NVS import slot. Mirrors
 * NVS_APP_VEH_BLOB_MAX in main/bsp_obd_dsp/nvs_app.h (that header pulls in
 * esp_err.h, so it is not included on the host); keep the two equal.
 */
#define VT_VEH_BLOB_MAX (8u * 1024u)

/* Read a whole file (malloc'd, NUL-terminated). Returns NULL on error. */
char *vt_read_file(const char *path, size_t *len);

/* malloc-backed allocator for vp_compile(). */
extern const vp_allocator_t vt_malloc_allocator;

/* Print issues as "  CODE path: msg". */
void vt_print_issues(const vp_issues_t *l, const char *prefix);
