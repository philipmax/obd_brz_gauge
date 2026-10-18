/* Expand omitted standard OBD2 signals from the generated built-in profile.
 * Explicit declarations (including disabled ones) replace the whole signal.
 * Keep this in the import path: the polling engine still sees one flat blob.
 */
#include "vp_internal.h"
#include "vp_builtin.h"

#include <string.h>

static const char *jstr(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static bool has_signal(const cJSON *signals, const char *id)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, signals) {
        const char *key = jstr(it, "id");
        if (key && !strcmp(key, id)) return true;
    }
    return false;
}

const cJSON *vp_int_resolve_defaults(const cJSON *root, cJSON **owned, vp_issues_t *issues)
{
    const vp_profile_t *builtin = vp_builtin_generic();
    const vp_signal_t *defs = vp_profile_signals(builtin);
    const cJSON *signals = cJSON_GetObjectItemCaseSensitive(root, "signals");
    const cJSON *policy_field = cJSON_GetObjectItemCaseSensitive(root, "defaults");
    const char *policy = jstr(root, "defaults");
    cJSON *base = NULL, *resolved = NULL, *dst;
    const cJSON *it;
    uint16_t i;
    bool missing = false;

    *owned = NULL;
    /* Leave malformed values untouched so the validator reports their paths. */
    if (!cJSON_IsObject(root) || (signals && !cJSON_IsArray(signals)) ||
        (policy_field && (!policy || strcmp(policy, "obd2"))) ||
        cJSON_GetArraySize(signals) > VP_MAX_SIGNALS) return root;
    for (i = 0; i < builtin->n_sig; i++) {
        /* Gear is derived from vehicle-specific ratios, not a standard PID. */
        if (defs[i].src_count && !has_signal(signals, defs[i].key)) missing = true;
    }
    if (!missing) return root;

    base = cJSON_ParseWithLength(vp_builtin_generic_json, vp_builtin_generic_json_len);
    resolved = cJSON_Duplicate(root, true);
    if (!base || !resolved) goto nomem;
    dst = cJSON_GetObjectItemCaseSensitive(resolved, "signals");
    if (!dst) {
        dst = cJSON_CreateArray();
        if (!dst) goto nomem;
        if (!cJSON_AddItemToObject(resolved, "signals", dst)) {
            cJSON_Delete(dst);
            goto nomem;
        }
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(base, "signals")) {
        const char *id = jstr(it, "id");
        cJSON *copy;
        if (!cJSON_GetObjectItemCaseSensitive(it, "src") || !id || has_signal(signals, id)) continue;
        copy = cJSON_Duplicate(it, true);
        if (!copy) goto nomem;
        if (!cJSON_AddItemToArray(dst, copy)) {
            cJSON_Delete(copy);
            goto nomem;
        }
    }
    cJSON_Delete(base);
    *owned = resolved;
    return resolved;

nomem:
    cJSON_Delete(base);
    cJSON_Delete(resolved);
    vp_issue_add(issues, "E_NOMEM", VP_SEV_ERROR, "$", "cannot expand OBD2 defaults");
    return NULL;
}
