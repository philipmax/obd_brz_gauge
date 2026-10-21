// Unit tests for the theme rules core: parsing edge cases, limits, allocator,
// colors, NaN handling and the label formatter's buffer behaviour. Behaviour
// that the editor defines is covered by the vector runners instead.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tr_eval.h"
#include "tr_format.h"
#include "tr_parse.h"
#include "test_util.h"

static int s_checks, s_failed;

#define CHECK(cond) do { \
    s_checks++; \
    if (!(cond)) { s_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static long s_live, s_total;
static bool s_oom;

static void *t_calloc(size_t n, size_t size, void *ctx)
{
    (void)ctx;
    if (s_oom) return NULL;
    void *p = calloc(n, size);
    if (p) { s_live++; s_total++; }
    return p;
}

static void t_free(void *p, void *ctx)
{
    (void)ctx;
    if (p) s_live--;
    free(p);
}

static tr_compile_cfg_t cfg_default(void)
{
    tr_compile_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.alloc.calloc_fn = t_calloc;
    cfg.alloc.free_fn = t_free;
    cfg.resolve_source = tu_resolve;
    return cfg;
}

static tr_program_t *compile_str(const char *json, const tr_compile_cfg_t *cfg, tr_diag_t *diag)
{
    tr_program_t *p = NULL;
    tr_err_t err = tr_compile_json(json, strlen(json), cfg, &p, diag);
    CHECK(err == TR_OK);
    return p;
}

static tr_inputs_t inputs(double *values, double elapsed)
{
    tr_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.values = values;
    in.n_values = TU_N_SOURCES;
    in.elapsed_ms = elapsed;
    return in;
}

static void test_colors(void)
{
    uint32_t c = 0;
    CHECK(tr_parse_color("#ff3158", &c) && c == 0xFF3158);
    CHECK(tr_parse_color("#FF3158", &c) && c == 0xFF3158);
    CHECK(tr_parse_color("0xFF3158", &c) && c == 0xFF3158);
    CHECK(tr_parse_color("0Xff3158", &c) && c == 0xFF3158);
    CHECK(tr_parse_color("ff3158", &c) && c == 0xFF3158);
    CHECK(tr_parse_color("#000000", &c) && c == 0);
    CHECK(tr_parse_color("#f0a", &c) && c == 0xFF00AA);
    CHECK(!tr_parse_color("f0a", &c));
    CHECK(!tr_parse_color("0xf0a", &c));
    CHECK(!tr_parse_color("red", &c));
    CHECK(!tr_parse_color("#ff31", &c));
    CHECK(!tr_parse_color("#ff315800", &c));
    CHECK(!tr_parse_color("#gg0000", &c));
    CHECK(!tr_parse_color("", &c));
    CHECK(!tr_parse_color("#", &c));
    CHECK(!tr_parse_color("0x", &c));
    CHECK(!tr_parse_color(" #ff3158", &c));
    CHECK(!tr_parse_color(NULL, &c));

    CHECK(tr_kind_from_str("arc") == TR_KIND_ARC);
    CHECK(tr_kind_from_str("pointer") == TR_KIND_POINTER);
    CHECK(tr_kind_from_str("Arc") == TR_KIND_UNKNOWN);
    CHECK(tr_kind_from_str(NULL) == TR_KIND_UNKNOWN);
}

static void test_limits(void)
{
    // 20 rules (+1 non-object), each 6 conditions and 10 actions
    cJSON *page = cJSON_CreateObject();
    cJSON *rules = cJSON_AddArrayToObject(page, "rules");
    cJSON_AddItemToArray(rules, cJSON_CreateNull());
    for (int r = 0; r < 20; r++) {
        cJSON *rule = cJSON_CreateObject();
        char id[16];
        snprintf(id, sizeof(id), "r%d", r);
        cJSON_AddStringToObject(rule, "id", id);
        cJSON *when = cJSON_AddObjectToObject(rule, "when");
        cJSON *conds = cJSON_AddArrayToObject(when, "conditions");
        for (int k = 0; k < 6; k++) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "source", "obd.rpm");
            cJSON_AddStringToObject(c, "op", ">=");
            cJSON_AddNumberToObject(c, "value", k);
            cJSON_AddItemToArray(conds, c);
        }
        cJSON *apply = cJSON_AddArrayToObject(rule, "apply");
        for (int a = 0; a < 10; a++) {
            cJSON *act = cJSON_CreateObject();
            char tid[16];
            snprintf(tid, sizeof(tid), "e%d", a);
            cJSON_AddStringToObject(act, "target", tid);
            cJSON_AddBoolToObject(cJSON_AddObjectToObject(act, "set"), "visible", 0);
            cJSON_AddItemToArray(apply, act);
        }
        cJSON_AddItemToArray(rules, rule);
    }
    tr_compile_cfg_t cfg = cfg_default();
    tr_program_t *p = NULL;
    tr_diag_t d;
    long before = s_total;
    CHECK(tr_compile(page, &cfg, &p, &d) == TR_OK);
    CHECK(s_total - before == 1);
    CHECK(p->n_rules == TR_MAX_RULES);
    CHECK(d.rules_in == 21);
    CHECK(d.rules_dropped == 5);
    CHECK(d.conds_dropped == TR_MAX_RULES * 2);
    CHECK(d.actions_dropped == TR_MAX_RULES * 2);
    CHECK(p->rules[0].n_conds == TR_MAX_CONDS && p->rules[15].n_actions == TR_MAX_ACTIONS);
    CHECK(strcmp(p->rules[15].id, "r15") == 0);
    CHECK(p->n_targets == TR_MAX_ACTIONS);   // e0..e7, deduplicated
    CHECK(d.unknown_targets == TR_MAX_ACTIONS);
    CHECK(p->targets[3].elem == TR_ELEM_NONE);

    double v[TU_N_SOURCES] = { 0 };
    v[0] = 3;   // satisfies conditions 0..3 (value 0..3)
    tr_inputs_t in = inputs(v, 0);
    const tr_result_t *res = tr_eval(p, &in);
    CHECK(res->n_active == TR_MAX_RULES);
    CHECK(res->n_targets == TR_MAX_ACTIONS);
    const tr_override_t *o = tr_result_override(p, 7);
    CHECK(o && (o->props & TR_PROP_VISIBLE) && o->visible == 0);
    tr_program_free(p);
    cJSON_Delete(page);
}

static void test_legacy_and_values(void)
{
    tr_compile_cfg_t cfg = cfg_default();
    tr_diag_t d;
    tr_program_t *p = compile_str(
        "{\"rules\":[{\"id\":\"a\",\"when\":{\"source\":\"obd.rpm\",\"op\":\">\",\"value\":1,"
        "\"compareSource\":\"obd.speed\"}},"
        "{\"id\":\"b\",\"when\":{\"conditions\":[{\"source\":\"obd.rpm\",\"op\":\">=\",\"value\":1,\"value\":5000}]}},"
        "{\"id\":\"c\",\"when\":{\"conditions\":[{\"source\":\"obd.fuel\",\"op\":\"!=\",\"value\":0,"
        "\"compareSource\":\"obd.nope\"}]}}]}", &cfg, &d);
    CHECK(p->n_rules == 3);
    const tr_cond_t *k = &p->rules[0].conds[0];
    CHECK(p->rules[0].n_conds == 1 && k->has_cmp && k->src == 0 && k->cmp_src == 1);
    CHECK(p->rules[1].conds[0].value == 5000.0);   // JSON.parse: last duplicate key wins
    CHECK(d.unknown_sources == 2);
    CHECK(p->rules[2].conds[0].src == TR_SRC_NONE && p->rules[2].conds[0].cmp_src == TR_SRC_NONE);

    double v[TU_N_SOURCES] = { 0 };
    v[0] = 4999;
    tr_inputs_t in = inputs(v, 0);
    CHECK(!tr_rule_active(p, 1, &in));
    v[0] = 5000;
    CHECK(tr_rule_active(p, 1, &in));
    CHECK(!tr_rule_active(p, 2, &in));  // undefined !== undefined is false
    CHECK(!tr_rule_active(p, 3, &in));  // out of range
    tr_program_free(p);
}

static void test_nan_inputs(void)
{
    tr_compile_cfg_t cfg = cfg_default();
    tr_program_t *p = compile_str(
        "{\"rules\":["
        "{\"id\":\"gt\",\"when\":{\"conditions\":[{\"source\":\"obd.rpm\",\"op\":\">\",\"value\":-1}]}},"
        "{\"id\":\"eq\",\"when\":{\"conditions\":[{\"source\":\"obd.rpm\",\"op\":\"==\",\"value\":0}]}},"
        "{\"id\":\"ne\",\"when\":{\"conditions\":[{\"source\":\"obd.rpm\",\"op\":\"!=\",\"value\":0}]}},"
        "{\"id\":\"hi\",\"when\":{\"conditions\":[{\"source\":\"obd.intake_temp\",\"op\":\"<\",\"value\":1}]}}"
        "]}", &cfg, NULL);
    double v[TU_N_SOURCES] = { 0 };
    v[0] = NAN;
    tr_inputs_t in = inputs(v, 0);
    CHECK(!tr_rule_active(p, 0, &in));
    CHECK(!tr_rule_active(p, 1, &in));
    CHECK(tr_rule_active(p, 2, &in));

    v[0] = 0;
    in.invalid_mask = 1;   // mask wins over a finite value
    CHECK(!tr_rule_active(p, 1, &in));
    CHECK(tr_rule_active(p, 2, &in));
    in.invalid_mask = 0;
    CHECK(tr_rule_active(p, 1, &in));

    in.n_values = 10;      // index 10 (intake_temp) beyond the table → NaN
    CHECK(!tr_rule_active(p, 3, &in));
    in.n_values = TU_N_SOURCES;
    CHECK(tr_rule_active(p, 3, &in));
    in.values = NULL;
    CHECK(!tr_rule_active(p, 3, &in));
    tr_program_free(p);
}

static const tr_element_t k_elems[] = {
    { "arc1", TR_KIND_ARC },
    { "lbl1", TR_KIND_LABEL },
    { "img1", TR_KIND_POINTER },
    { "lbl1", TR_KIND_IMAGE },   // duplicate id: first wins
};

static void test_actions_and_element_table(void)
{
    tr_compile_cfg_t cfg = cfg_default();
    cfg.elements = k_elems;
    cfg.n_elements = 4;
    tr_diag_t d;
    tr_program_t *p = compile_str(
        "{\"elements\":[{\"id\":\"arc1\",\"kind\":\"label\"}],"
        "\"rules\":[{\"id\":\"r\",\"when\":{\"conditions\":[{\"source\":\"obd.rpm\",\"op\":\">=\",\"value\":0}]},"
        "\"apply\":["
        "{\"target\":\"arc1\",\"set\":{\"color\":\"0x00FF00\"}},"
        "{\"target\":\"lbl1\",\"set\":{\"color\":\"red\",\"text\":5,\"asset\":null,\"visible\":null}},"
        "{\"target\":\"img1\",\"set\":{\"color\":\"#0000ff\",\"asset\":\"needle_b\"}},"
        "{\"target\":7,\"set\":{\"color\":\"#0000ff\"}},"
        "{\"set\":{\"color\":\"#0000ff\"}},"
        "{\"target\":\"ghost\",\"set\":{\"text\":\"HOT\"}}]}]}", &cfg, &d);
    CHECK(p->rules[0].n_actions == 4);
    CHECK(d.actions_invalid == 2);
    CHECK(d.bad_values == 3);
    CHECK(d.unknown_targets == 1);
    CHECK(p->n_targets == 4);
    CHECK(p->targets[0].kind == TR_KIND_ARC && p->targets[0].elem == 0);   // table beats page.elements
    CHECK(p->targets[1].kind == TR_KIND_LABEL && p->targets[1].elem == 1);
    CHECK(p->targets[2].kind == TR_KIND_POINTER && p->targets[2].elem == 2);
    CHECK(p->targets[3].elem == TR_ELEM_NONE && p->targets[3].kind == TR_KIND_UNKNOWN);

    double v[TU_N_SOURCES] = { 0 };
    tr_inputs_t in = inputs(v, 0);
    tr_eval(p, &in);
    const tr_override_t *o = tr_result_override(p, 0);
    CHECK(o && o->props == TR_PROP_COLOR && o->color == 0x00FF00);
    o = tr_result_override(p, 1);
    CHECK(o && o->props == TR_PROP_VISIBLE && o->visible == 1);
    o = tr_result_override(p, 2);
    CHECK(o && o->props == (TR_PROP_TINT | TR_PROP_ASSET) && o->color == 0x0000FF &&
          strcmp(o->asset, "needle_b") == 0);
    o = tr_result_override(p, 3);
    CHECK(o && o->props == TR_PROP_TEXT && strcmp(o->text, "HOT") == 0);
    CHECK(tr_result_override(p, 4) == NULL);

    v[0] = -1;   // rule off → back to base: nothing touched
    tr_eval(p, &in);
    CHECK(tr_result_override(p, 0) == NULL && tr_result_override(p, 3) == NULL);
    tr_program_free(p);
}

static void test_action_animation(void)
{
    tr_compile_cfg_t cfg = cfg_default();
    cfg.elements = k_elems;
    cfg.n_elements = 4;
    tr_program_t *p = compile_str(
        "{\"rules\":[{\"id\":\"fade\",\"trigger\":{\"type\":\"change\",\"source\":\"obd.speed\"},"
        "\"apply\":[{\"target\":\"arc1\",\"set\":{\"color\":\"#ffffff\"},"
        "\"animation\":{\"mode\":\"keyframes\",\"durationMs\":1000,"
        "\"keyframes\":[{\"at\":0,\"value\":0},{\"at\":50,\"value\":80},{\"at\":100,\"value\":100}]}}]}]}",
        &cfg, NULL);
    CHECK(p->rules[0].transient_hold_ms == 1000.0);
    CHECK(p->rules[0].actions[0].animation.mode == 2);
    CHECK(p->rules[0].actions[0].animation.n_frames == 3);

    double v[TU_N_SOURCES] = { 0 };
    tr_inputs_t in = inputs(v, 1000.0);
    in.changed_mask = UINT64_C(1) << 1;
    const tr_result_t *res = tr_eval(p, &in);
    const tr_override_t *o = tr_result_override(p, 0);
    CHECK(res->n_active == 1 && o && o->animated_color);
    CHECK(o && o->animation_progress == 0.0);

    in.changed_mask = 0;
    in.elapsed_ms = 1250.0;
    res = tr_eval(p, &in);
    o = tr_result_override(p, 0);
    CHECK(res->n_active == 1 && o && fabs(o->animation_progress - 0.4) < 1e-9);

    in.elapsed_ms = 2000.0;
    res = tr_eval(p, &in);
    CHECK(res->n_active == 0 && tr_result_override(p, 0) == NULL);
    tr_program_free(p);

    p = compile_str(
        "{\"rules\":[{\"id\":\"timer\",\"trigger\":{\"type\":\"timer\",\"intervalMs\":250}}]}",
        &cfg, NULL);
    in = inputs(v, -100.0);
    CHECK(tr_eval(p, &in)->n_active == 0);
    tr_program_free(p);
}

static void test_opacity_animation_and_exit(void)
{
    tr_compile_cfg_t cfg = cfg_default();
    cfg.elements = k_elems;
    cfg.n_elements = 4;
    tr_program_t *p = compile_str(
        "{\"rules\":[{\"id\":\"opacity\","
        "\"when\":{\"conditions\":[{\"source\":\"obd.rpm\",\"op\":\">=\",\"value\":4000}]},"
        "\"apply\":[{\"target\":\"img1\",\"set\":{\"opacity\":0},"
        "\"animation\":{\"mode\":\"fade\",\"durationMs\":1000}}]}]}",
        &cfg, NULL);

    double values[TU_N_SOURCES] = { 4000 };
    tr_inputs_t in = inputs(values, 0.0);
    const tr_result_t *res = tr_eval(p, &in);
    const tr_override_t *o = tr_result_override(p, 0);
    CHECK(res->n_active == 1 && o && o->animated_opacity);
    CHECK(o && o->opacity == 0.0 && o->opacity_progress == 0.0);

    in.elapsed_ms = 500.0;
    tr_eval(p, &in);
    o = tr_result_override(p, 0);
    CHECK(o && fabs(o->opacity_progress - 0.5) < 1e-9);

    in.elapsed_ms = 1000.0;
    tr_eval(p, &in);
    o = tr_result_override(p, 0);
    CHECK(o && o->opacity_progress == 1.0 && !o->animated_opacity);

    values[0] = 0;
    in.elapsed_ms = 1200.0;
    res = tr_eval(p, &in);
    o = tr_result_override(p, 0);
    CHECK(res->n_active == 0 && p->resolved_exit_mask != 0);
    CHECK(o && o->animated_opacity && o->opacity_progress == 1.0);

    in.elapsed_ms = 1700.0;
    tr_eval(p, &in);
    o = tr_result_override(p, 0);
    CHECK(o && fabs(o->opacity_progress - 0.5) < 1e-9);

    in.elapsed_ms = 2200.0;
    res = tr_eval(p, &in);
    CHECK(res->n_active == 0 && p->resolved_exit_mask == 0);
    CHECK(tr_result_override(p, 0) == NULL);
    tr_program_free(p);

    p = compile_str(
        "{\"rules\":[{\"id\":\"opacity-mid-exit\","
        "\"when\":{\"conditions\":[{\"source\":\"obd.rpm\",\"op\":\">=\",\"value\":4000}]},"
        "\"apply\":[{\"target\":\"img1\",\"set\":{\"opacity\":0},"
        "\"animation\":{\"mode\":\"fade\",\"durationMs\":1000}}]}]}",
        &cfg, NULL);
    values[0] = 5000;
    in = inputs(values, 0.0);
    tr_eval(p, &in);
    in.elapsed_ms = 500.0;
    tr_eval(p, &in);
    o = tr_result_override(p, 0);
    CHECK(o && fabs(o->opacity_progress - 0.5) < 1e-9);

    values[0] = 0;
    in.elapsed_ms = 600.0;
    tr_eval(p, &in);
    o = tr_result_override(p, 0);
    CHECK(o && fabs(o->opacity_progress - 0.6) < 1e-9);
    in.elapsed_ms = 1100.0;
    tr_eval(p, &in);
    o = tr_result_override(p, 0);
    CHECK(o && fabs(o->opacity_progress - 0.3) < 1e-9);
    in.elapsed_ms = 1600.0;
    res = tr_eval(p, &in);
    CHECK(res->n_active == 0 && p->resolved_exit_mask == 0);
    CHECK(tr_result_override(p, 0) == NULL);
    tr_program_free(p);
}

static void test_change_mask_and_ids(void)
{
    char json[512];
    char longid[101];
    memset(longid, 'k', 100);
    longid[100] = '\0';
    snprintf(json, sizeof(json),
             "{\"rules\":[{\"id\":\"%s\",\"trigger\":{\"type\":\"change\",\"source\":\"ext.x\"},\"when\":{}},"
             "{\"id\":\"%s\",\"trigger\":{\"type\":\"change\"},\"when\":{}},"
             "{\"trigger\":{\"type\":\"change\",\"source\":\"obd.speed\"}}]}", longid, longid);
    tr_compile_cfg_t cfg = cfg_default();
    tr_diag_t d;
    tr_program_t *p = compile_str(json, &cfg, &d);
    CHECK(p->n_rules == 3);
    CHECK(strlen(p->rules[0].id) == 100);
    CHECK(p->rules[1].slot == 0 && d.shared_ids == 1);
    CHECK(strcmp(p->rules[2].id, "") == 0 && p->rules[2].slot == 2);   // missing when: {}
    CHECK(p->rules[0].change_src == TR_SRC_NONE && d.unknown_sources == 1);
    CHECK(p->rules[1].change_src == 0);
    double v[TU_N_SOURCES] = { 0 };
    tr_inputs_t in = inputs(v, 0);
    in.changed_mask = ~0ull;
    CHECK(!tr_rule_active(p, 0, &in));
    CHECK(tr_rule_active(p, 1, &in));
    CHECK(tr_rule_active(p, 2, &in));
    tr_program_free(p);
}

static int resolve_high(const char *key, void *ctx)
{
    (void)ctx;
    return strcmp(key, "ext.far") == 0 ? 70 : tu_resolve(key, NULL);
}

static void test_change_unmasked(void)
{
    tr_compile_cfg_t cfg = cfg_default();
    cfg.resolve_source = resolve_high;
    tr_diag_t d;
    tr_program_t *p = compile_str(
        "{\"rules\":[{\"id\":\"c\",\"trigger\":{\"type\":\"change\",\"source\":\"ext.far\"},\"when\":{}},"
        "{\"id\":\"v\",\"when\":{\"conditions\":[{\"source\":\"ext.far\",\"op\":\">\",\"value\":1}]}}]}",
        &cfg, &d);
    CHECK(d.change_unmasked == 1);
    CHECK(p->rules[0].change_src == 70);
    double v[80] = { 0 };
    v[70] = 2;
    tr_inputs_t in = inputs(v, 0);
    in.n_values = 80;
    in.changed_mask = ~0ull;
    in.invalid_mask = ~0ull;   // mask only covers indices < 64
    CHECK(!tr_rule_active(p, 0, &in));
    CHECK(tr_rule_active(p, 1, &in));
    tr_program_free(p);
}

static void test_alloc_and_errors(void)
{
    tr_compile_cfg_t cfg = cfg_default();
    tr_program_t *p = (tr_program_t *)&cfg;   // must be overwritten
    tr_diag_t d;
    CHECK(tr_compile_json("{\"rules\":[", 10, &cfg, &p, &d) == TR_ERR_JSON && p == NULL);
    CHECK(tr_compile_json("[1,2]", 5, &cfg, &p, &d) == TR_ERR_JSON && p == NULL);
    CHECK(tr_compile(NULL, &cfg, &p, &d) == TR_ERR_JSON);
    CHECK(tr_compile_json("{}", 2, NULL, &p, &d) == TR_ERR_ARG);

    // not NUL-terminated: trailing garbage outside len is ignored
    const char buf[] = "{\"rules\":[]}XXXX";
    CHECK(tr_compile_json(buf, 12, &cfg, &p, &d) == TR_OK && p && p->n_rules == 0);
    double v[TU_N_SOURCES] = { 0 };
    tr_inputs_t in = inputs(v, 0);
    const tr_result_t *res = tr_eval(p, &in);
    CHECK(res && res->n_active == 0 && res->n_targets == 0);
    tr_program_free(p);

    s_oom = true;
    CHECK(tr_compile_json("{\"rules\":[]}", 12, &cfg, &p, &d) == TR_ERR_NOMEM && p == NULL);
    s_oom = false;

    // default allocator
    tr_compile_cfg_t plain;
    memset(&plain, 0, sizeof(plain));
    const char *one = "{\"rules\":[{\"id\":\"x\",\"when\":{\"source\":\"obd.rpm\",\"op\":\">\",\"value\":1}}]}";
    CHECK(tr_compile_json(one, strlen(one), &plain, &p, &d) == TR_OK);
    CHECK(p->rules[0].conds[0].src == TR_SRC_NONE);   // no resolver: every key unknown
    tr_program_free(p);
    tr_program_free(NULL);
    CHECK(tr_eval(NULL, &in) == NULL);
}

static void test_reset_restarts_latch(void)
{
    tr_compile_cfg_t cfg = cfg_default();
    tr_program_t *p = compile_str(
        "{\"rules\":[{\"id\":\"h\",\"debounceMs\":100,\"when\":{\"hysteresis\":100,"
        "\"conditions\":[{\"source\":\"obd.rpm\",\"op\":\">=\",\"value\":1000}]}}]}", &cfg, NULL);
    double v[TU_N_SOURCES] = { 0 };
    tr_inputs_t in = inputs(v, 0);
    v[0] = 2000;
    CHECK(tr_eval(p, &in)->n_active == 0);   // debounce armed at 0
    v[0] = 950;
    in.elapsed_ms = 100;
    CHECK(tr_eval(p, &in)->n_active == 1);   // latched true inside the band, 100 ms elapsed
    tr_reset(p);
    CHECK(p->state.latch[0][0] == -1 && p->state.deb_armed[0] == 0);
    in.elapsed_ms = 200;
    CHECK(tr_eval(p, &in)->n_active == 0);   // first eval after reset is raw: 950 < 1000
    tr_program_free(p);
}

static void test_format_buffers(void)
{
    char out[TR_FMT_BUF];
    double a[2] = { 42, 7 };
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%d/%d", a, 2) == 4 && strcmp(out, "42/7") == 0);
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%d %d %d", a, 2) == 6 && strcmp(out, "42 7 0") == 0);
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%d", NULL, 0) == 1 && strcmp(out, "0") == 0);
    CHECK(tr_fmt_int_printf(out, sizeof(out), NULL, a, 2) == 0 && out[0] == '\0');
    CHECK(tr_fmt_int_printf(out, sizeof(out), "ab%", a, 2) == 4 && strcmp(out, "ab42") == 0);

    // truncation never splits a UTF-8 sequence
    char small[6];
    CHECK(tr_fmt_int_printf(small, 5, "%d中", a, 1) == 2 && strcmp(small, "42") == 0);
    CHECK(tr_fmt_int_printf(small, 6, "%d中x", a, 1) == 5 && strcmp(small, "42中") == 0);
    CHECK(tr_fmt_int_printf(small, 1, "%d", a, 1) == 0 && small[0] == '\0');
    CHECK(tr_fmt_int_printf(NULL, 4, "%d", a, 1) == 0);

    // huge width is bounded by the buffer
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%99999999999d", a, 1) == sizeof(out) - 1);

    // NaN / Infinity / %c NUL / multi-byte conversion char
    double nanv[1] = { NAN };
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%d|%x|%X|%5d", nanv, 1) > 0 &&
          strcmp(out, "NaN|0|0|    0") == 0);
    double vals[3] = { NAN, NAN, NAN };
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%d|%x|%X", vals, 3) > 0 && strcmp(out, "NaN|NaN|NAN") == 0);
    double inf[3] = { INFINITY, -INFINITY, -INFINITY };
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%05d|%d|%x", inf, 3) > 0 &&
          strcmp(out, "Infinity|-Infinity|0") == 0);
    double zero[1] = { 0 };
    CHECK(tr_fmt_int_printf(out, sizeof(out), "[%3c]", zero, 1) == 4 && strcmp(out, "[  ]") == 0);
    double sur[1] = { 0xD800 };
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%c", sur, 1) == 3 && strcmp(out, "\xEF\xBF\xBD") == 0);
    CHECK(tr_fmt_int_printf(out, sizeof(out), "%°C", a, 1) == 3 && strcmp(out, "42C") == 0);

    CHECK(tr_fmt_is_int_only("%d.%02d"));
    CHECK(tr_fmt_is_int_only("100%%"));
    CHECK(!tr_fmt_is_int_only("%f"));
    CHECK(!tr_fmt_is_int_only("%5"));
    CHECK(!tr_fmt_is_int_only("%"));
    CHECK(!tr_fmt_is_int_only(NULL));

    CHECK(tr_fmt_label(out, sizeof(out), NULL, 0, 3500) == 4 && strcmp(out, "3500") == 0);
    CHECK(tr_fmt_label(out, sizeof(out), "%d.%d", NAN, 136) > 0 && strcmp(out, "136.0") == 0);
    CHECK(tr_fmt_label(out, sizeof(out), "%d.%02d", 100, NAN) > 0 && strcmp(out, "NaN.NaN") == 0);
}

int main(void)
{
    test_colors();
    test_limits();
    test_legacy_and_values();
    test_nan_inputs();
    test_actions_and_element_table();
    test_action_animation();
    test_opacity_animation_and_exit();
    test_change_mask_and_ids();
    test_change_unmasked();
    test_alloc_and_errors();
    test_reset_restarts_latch();
    test_format_buffers();
    if (s_live != 0) {
        s_failed++;
        printf("FAIL %ld allocation(s) leaked\n", s_live);
    }
    printf("unit checks=%d fail=%d\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
