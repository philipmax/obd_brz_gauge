#pragma once
/* Shared helpers for the vehicle host tests and tools. */

#include <stddef.h>

#include "vp_validate.h"
#include "vp_profile.h"

/*
 * Fallback signal registry (obd.* keys and numeric ids from the event-system
 * spec). Tools prefer contracts/signals.json via vt_registry_load().
 */
extern const vp_sig_def_t vt_default_registry[];
extern const size_t vt_default_registry_count;

typedef struct {
    vp_sig_def_t *defs;
    size_t        n;
    char         *strings;   /* backing store for keys */
} vt_registry_t;

/*
 * Load obd.* keys from a signals.json: either an array of entries or an
 * object holding such an array under "signals". An entry is a string key
 * or an object with "key" (or an "id" that starts with "obd.") and an
 * optional numeric id in "num_id" or "id" (number or "0x..." string).
 * Returns 0 on success, -1 on error (message printed).
 */
int vt_registry_load(const char *path, vt_registry_t *reg);
void vt_registry_free(vt_registry_t *reg);

/* Read a whole file (malloc'd, NUL-terminated). Returns NULL on error. */
char *vt_read_file(const char *path, size_t *len);

/* malloc-backed allocator for vp_compile(). */
extern const vp_allocator_t vt_malloc_allocator;

/* Print issues as "  CODE path: msg". */
void vt_print_issues(const vp_issues_t *l, const char *prefix);
