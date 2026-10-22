// Runs rule vectors (format of the golden contracts/fixtures/rules/vectors.json,
// produced by run.ts against the editor's ruleEngine.ts) through the C core.
//
//   test_rules_vectors [--expect-groups N] [--expect-steps M] <vectors.json> [fallback...]
//
// The first existing path is used. Step semantics follow run.ts: values are
// merged over all-zero sources, runtime "shared" keeps state across steps,
// "none" starts each step from a fresh runtime. Steps may also carry
// "invalid": [keys] (extra vectors), read as NaN.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tr_eval.h"
#include "tr_parse.h"
#include "test_util.h"

static long s_live_allocs;
static long s_total_allocs;

static void *count_calloc(size_t n, size_t size, void *ctx)
{
    (void)ctx;
    void *p = calloc(n, size);
    if (p) {
        s_live_allocs++;
        s_total_allocs++;
    }
    return p;
}

static void count_free(void *p, void *ctx)
{
    (void)ctx;
    if (p) s_live_allocs--;
    free(p);
}

static const char *str_or(const cJSON *it, const char *dflt)
{
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : dflt;
}

static void build_inputs(const cJSON *step, double *values, tr_inputs_t *in)
{
    memset(in, 0, sizeof(*in));
    for (int i = 0; i < TU_N_SOURCES; i++) values[i] = 0.0;

    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(step, "values")) {
        int idx = tu_resolve(it->string, NULL);
        if (idx >= 0 && cJSON_IsNumber(it)) values[idx] = it->valuedouble;
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(step, "invalid")) {
        int idx = cJSON_IsString(it) ? tu_resolve(it->valuestring, NULL) : -1;
        if (idx >= 0) in->invalid_mask |= 1ull << idx;
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(step, "changed")) {
        int idx = cJSON_IsString(it) ? tu_resolve(it->valuestring, NULL) : -1;
        if (idx >= 0) in->changed_mask |= 1ull << idx;
    }
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(step, "elapsedMs");
    in->elapsed_ms = cJSON_IsNumber(e) ? e->valuedouble : 0.0;
    in->values = values;
    in->n_values = TU_N_SOURCES;
}

// Same shape as the editor's { activeRules: ids, overrides }.
static cJSON *result_json(const tr_program_t *prog, const tr_result_t *res, cJSON **overrides_out, bool *colors_ok)
{
    cJSON *active = cJSON_CreateArray();
    for (uint8_t i = 0; i < res->n_active; i++) {
        cJSON_AddItemToArray(active, cJSON_CreateString(prog->rules[res->active[i]].id));
    }
    cJSON *ovr = cJSON_CreateObject();
    for (uint16_t t = 0; t < prog->n_targets; t++) {
        const tr_override_t *o = tr_result_override(prog, t);
        if (!o) continue;
        cJSON *e = cJSON_CreateObject();
        if (o->props & TR_PROP_ANY_COLOR) {
            uint32_t rgb = 0;
            if (!tr_parse_color(o->color_str, &rgb) || rgb != o->color) *colors_ok = false;
        }
        if (o->props & TR_PROP_COLOR) cJSON_AddStringToObject(e, "color", o->color_str);
        if (o->props & TR_PROP_BG_COLOR) cJSON_AddStringToObject(e, "bgColor", o->color_str);
        if (o->props & TR_PROP_TINT) cJSON_AddStringToObject(e, "tintColor", o->color_str);
        if (o->props & TR_PROP_TEXT) cJSON_AddStringToObject(e, "text", o->text);
        if (o->props & TR_PROP_VISIBLE) cJSON_AddBoolToObject(e, "visible", o->visible);
        if (o->props & TR_PROP_ASSET) cJSON_AddStringToObject(e, "assetName", o->asset);
        cJSON_AddItemToObject(ovr, prog->targets[t].id, e);
    }
    *overrides_out = ovr;
    return active;
}

static void print_json(const char *label, const cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    printf("    %s %s\n", label, s ? s : "?");
    free(s);
}

int main(int argc, char **argv)
{
    int expect_groups = -1, expect_steps = -1;
    const char *paths[16];
    int n_paths = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--expect-groups") == 0 && i + 1 < argc) expect_groups = atoi(argv[++i]);
        else if (strcmp(argv[i], "--expect-steps") == 0 && i + 1 < argc) expect_steps = atoi(argv[++i]);
        else if (n_paths < 15) paths[n_paths++] = argv[i];
    }
    paths[n_paths] = NULL;
    const char *path = tu_first_existing(paths);
    if (!path) {
        fprintf(stderr, "no vectors file found\n");
        return 2;
    }
    cJSON *vectors = tu_load_json(path);
    if (!cJSON_IsArray(vectors)) return 2;
    printf("vectors: %s\n", path);

    int groups = 0, pass = 0, fail = 0;
    const cJSON *vec;
    cJSON_ArrayForEach(vec, vectors) {
        groups++;
        const char *name = str_or(cJSON_GetObjectItemCaseSensitive(vec, "name"), "?");
        const char *api = str_or(cJSON_GetObjectItemCaseSensitive(vec, "api"), "");
        bool shared = strcmp(str_or(cJSON_GetObjectItemCaseSensitive(vec, "runtime"), ""), "shared") == 0;
        bool single = strcmp(api, "isRuleActive") == 0;

        cJSON *owned_page = NULL;
        const cJSON *page;
        if (single) {
            owned_page = cJSON_CreateObject();
            cJSON_AddItemToObject(owned_page, "elements", cJSON_CreateArray());
            cJSON *rules = cJSON_AddArrayToObject(owned_page, "rules");
            cJSON_AddItemToArray(rules, cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(vec, "rule"), 1));
            page = owned_page;
        } else {
            page = cJSON_GetObjectItemCaseSensitive(vec, "page");
        }

        tr_compile_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.alloc.calloc_fn = count_calloc;
        cfg.alloc.free_fn = count_free;
        cfg.resolve_source = tu_resolve;

        tr_program_t *prog = NULL;
        tr_diag_t diag;
        long before = s_total_allocs;
        tr_err_t err = tr_compile(page, &cfg, &prog, &diag);
        const cJSON *steps = cJSON_GetObjectItemCaseSensitive(vec, "steps");
        if (err != TR_OK || s_total_allocs - before != 1) {
            printf("FAIL %s: compile err=%d allocs=%ld\n", name, (int)err, s_total_allocs - before);
            fail += cJSON_GetArraySize(steps);
            cJSON_Delete(owned_page);
            continue;
        }

        int si = 0;
        const cJSON *step;
        cJSON_ArrayForEach(step, steps) {
            double values[TU_N_SOURCES];
            tr_inputs_t in;
            build_inputs(step, values, &in);
            if (!shared) tr_reset(prog);
            const cJSON *expect = cJSON_GetObjectItemCaseSensitive(step, "expect");
            bool ok = true;

            if (single) {
                bool got = tr_rule_active(prog, 0, &in);
                bool want = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(expect, "active"));
                ok = got == want;
                if (!ok) printf("FAIL %s step %d: active got %d want %d\n", name, si, got, want);
            } else {
                const tr_result_t *res = tr_eval(prog, &in);
                cJSON *ovr = NULL;
                bool colors_ok = true;
                cJSON *active = result_json(prog, res, &ovr, &colors_ok);
                const cJSON *want_active = cJSON_GetObjectItemCaseSensitive(expect, "activeRules");
                const cJSON *want_ovr = cJSON_GetObjectItemCaseSensitive(expect, "overrides");
                if (want_active && !cJSON_Compare(active, want_active, 1)) ok = false;
                if (want_ovr && !cJSON_Compare(ovr, want_ovr, 1)) ok = false;
                if (!colors_ok) ok = false;
                if (!ok) {
                    printf("FAIL %s step %d%s\n", name, si, colors_ok ? "" : " (rgb mismatch)");
                    print_json("got activeRules", active);
                    print_json("got overrides  ", ovr);
                    print_json("expect         ", expect);
                }
                cJSON_Delete(active);
                cJSON_Delete(ovr);
            }
            if (ok) pass++;
            else fail++;
            si++;
        }
        tr_program_free(prog);
        cJSON_Delete(owned_page);
    }
    cJSON_Delete(vectors);

    int rc = fail ? 1 : 0;
    printf("vectors=%d steps pass=%d fail=%d\n", groups, pass, fail);
    if (expect_groups >= 0 && groups != expect_groups) {
        printf("FAIL expected %d groups, file has %d\n", expect_groups, groups);
        rc = 1;
    }
    if (expect_steps >= 0 && pass + fail != expect_steps) {
        printf("FAIL expected %d steps, file has %d\n", expect_steps, pass + fail);
        rc = 1;
    }
    if (s_live_allocs != 0) {
        printf("FAIL %ld program allocation(s) leaked\n", s_live_allocs);
        rc = 1;
    }
    return rc;
}
