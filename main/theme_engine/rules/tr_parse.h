#pragma once
// ============================================================
//  tr_parse.h — compile the editor's page rules into a program
//
//  Input is the unchanged editor JSON: a GaugePage-like object
//  { "elements": [{ "id", "kind" }...], "rules": [GaugeRule...] }
//  with camelCase fields, including the legacy single-condition
//  form (when.source/op/value/compareSource) and '#RRGGBB' colors.
//  No editor-panel migration is applied.
// ============================================================

#include "tr_types.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    tr_alloc_t alloc;              // zeroed → calloc/free
    tr_resolve_fn resolve_source;  // NULL → every key unknown
    void *resolve_ctx;
    // Element table for target kinds. NULL → read page.elements[] from the JSON.
    // First entry wins for duplicate ids, like Array.prototype.find.
    const tr_element_t *elements;
    uint16_t n_elements;
} tr_compile_cfg_t;

// Compile page->rules. On success *out owns one allocation; free with
// tr_program_free(). diag may be NULL.
tr_err_t tr_compile(const cJSON *page, const tr_compile_cfg_t *cfg,
                    tr_program_t **out, tr_diag_t *diag);

// Same, from a JSON buffer (need not be NUL-terminated).
tr_err_t tr_compile_json(const char *json, size_t len, const tr_compile_cfg_t *cfg,
                         tr_program_t **out, tr_diag_t *diag);

void tr_program_free(tr_program_t *prog);

// Accepts "#RRGGBB", "0xRRGGBB", bare "RRGGBB" (any case) and CSS "#RGB".
bool tr_parse_color(const char *s, uint32_t *rgb);

tr_kind_t tr_kind_from_str(const char *s);

#ifdef __cplusplus
}
#endif
