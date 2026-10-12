#pragma once
// Shared helpers for the theme-rules host tests.

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

// The editor's 11 data sources (gaugeModel.ts DATA_SOURCES) → index 0..10.
#define TU_N_SOURCES 11
extern const char *const tu_sources[TU_N_SOURCES];
int tu_resolve(const char *key, void *ctx);

// First existing path of a NULL-terminated list, or NULL.
const char *tu_first_existing(const char *const *paths);

// Reads and parses a JSON file; NULL on failure (message printed).
cJSON *tu_load_json(const char *path);
