/* Sparse imports must produce the same standard requests as Generic OBD2. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "vp_builtin.h"
#include "vt.h"
#include "vt_support.h"

#define HEAD "\"schema\":\"skygauge.vehicle/1\",\"id\":\"test.sparse\",\"rev\":1,\"name\":\"Sparse\""
static const char minimal[] = "{" HEAD "}";

static vp_compile_err_t compile(const char *json, vp_profile_t **out, vp_issues_t *issues)
{
    return vp_compile_json(json, strlen(json), vp_validate_default_cfg(),
                           &vt_malloc_allocator, out, issues);
}

static const vp_request_t *request(const vp_profile_t *p, const char *key)
{
    int i = vp_profile_find_signal(p, key);
    const vp_signal_t *s;
    if (i < 0) return NULL;
    s = &vp_profile_signals(p)[i];
    if (!s->src_count) return NULL;
    return &vp_profile_requests(p)[vp_profile_sources(p)[s->src_first].ref];
}

static void test_minimal(void)
{
    static const char *const variants[] = {
        minimal, "{" HEAD ",\"signals\":[]}",
        "{" HEAD ",\"defaults\":\"obd2\",\"bus\":{}}",
        "{" HEAD ",\"bus\":{\"proto\":0}}",
        "{" HEAD ",\"bus\":{\"proto\":6}}"
    };
    size_t i;
    for (i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        vp_profile_t *p = NULL;
        const vp_profile_t *base = vp_builtin_generic();
        VT_CHECK_INT(compile(variants[i], &p, NULL), VP_COMPILE_OK);
        if (!p) continue;
        VT_CHECK(vp_profile_check(p, p->total_size));
        VT_CHECK_INT(p->n_sig, 10);
        VT_CHECK_INT(p->n_req, 10);
        VT_CHECK_INT(p->gear_count, 0);
        VT_CHECK_INT(p->proto, i == 4 ? 6 : 0);
        VT_CHECK_INT(p->hdr, 0x7E0);
        VT_CHECK_INT(p->atst, VP_DEFAULT_ATST);
        VT_CHECK_INT(p->gap_ms, VP_DEFAULT_GAP_MS);
        VT_CHECK(vp_profile_find_signal(p, "obd.gear") < 0);
        VT_CHECK(!memcmp(vp_profile_signals(p), vp_profile_signals(base),
                         10 * sizeof(vp_signal_t)));
        for (unsigned r = 0; r < p->n_req; r++) {
            vp_request_t expected = vp_profile_requests(base)[r];
            /* Sparse profiles inherit signals, not the generic bus header. */
            expected.hdr = p->hdr;
            expected.flags &= (uint8_t)~VP_REQ_F_FUNC;
            expected.rsv &= (uint8_t)~VP_REQ_RSV_HDR_EXPLICIT;
            VT_CHECK(!memcmp(&vp_profile_requests(p)[r], &expected, sizeof(expected)));
        }
        vp_profile_free(p, &vt_malloc_allocator);
    }
}

static void test_override_and_disable(void)
{
    static const char json[] = "{" HEAD ",\"bus\":{\"proto\":7,\"hdr\":\"18DA10F1\"},\"signals\":["
        "{\"id\":\"obd.coolant_temp\",\"disabled\":true},"
        "{\"id\":\"obd.oil_temp\",\"period_ms\":500,\"src\":[{\"req\":\"221234\","
        "\"hdr\":\"18DA18F1\",\"len\":1,\"rpn\":[[\"A\",0]]}]}]}";
    vp_profile_t *p = NULL;
    const vp_request_t *r;
    const vp_signal_t *s;
    const vp_source_t *src;
    unsigned i;
    VT_CHECK_INT(compile(json, &p, NULL), VP_COMPILE_OK);
    if (!p) return;
    VT_CHECK(vp_profile_check(p, p->total_size));
    VT_CHECK_INT(p->n_sig, 9);
    VT_CHECK_INT(p->n_req, 9);
    VT_CHECK(vp_profile_find_signal(p, "obd.coolant_temp") < 0);
    VT_CHECK_INT(vp_profile_find_signal(p, "obd.oil_temp"), 0);
    s = vp_profile_signals(p);
    VT_CHECK_INT(s[0].src_count, 1);
    VT_CHECK_INT(s[0].period_ms, 500);
    VT_CHECK_INT(s[0].filter, VP_FILTER_NONE); /* no field merge */
    r = request(p, "obd.oil_temp");
    VT_CHECK(r != NULL);
    if (r) {
        VT_CHECK_INT(r->hdr, VP_ID_29BIT | 0x18DA18F1u);
        VT_CHECK_INT(r->tx_len, 3);
        VT_CHECK_INT(r->tx[0], 0x22);
        VT_CHECK_INT(r->tx[1], 0x12);
        VT_CHECK_INT(r->tx[2], 0x34);
    }
    r = request(p, "obd.rpm");
    VT_CHECK(r != NULL);
    if (r) VT_CHECK_INT(r->hdr, VP_ID_29BIT | 0x18DA10F1u);
    src = vp_profile_sources(p);
    for (i = 0; i < p->n_sig; i++) {
        VT_CHECK_INT(src[s[i].src_first].sig, i); /* disabled indices are compacted */
    }
    vp_profile_free(p, &vt_malloc_allocator);
}

static void expect_invalid(const char *json, const char *code)
{
    vp_issue_t store[32];
    vp_issues_t issues;
    vp_profile_t *p = NULL;
    size_t i;
    bool found = false;
    vp_issues_init(&issues, store, 32);
    VT_CHECK_INT(compile(json, &p, &issues), VP_COMPILE_ERR_INVALID);
    VT_CHECK(p == NULL);
    for (i = 0; i < issues.count; i++) if (!strcmp(store[i].code, code)) found = true;
    VT_CHECK_MSG(found, "expected %s in %s", code, json);
    vp_profile_free(p, &vt_malloc_allocator);
}

static void test_policy_and_validation(void)
{
    vp_profile_t *p = NULL;
    cJSON *root = cJSON_Parse(minimal);
    char *before = cJSON_PrintUnformatted(root), *after;
    VT_CHECK_INT(vp_compile(root, vp_validate_default_cfg(), &vt_malloc_allocator, &p, NULL), VP_COMPILE_OK);
    after = cJSON_PrintUnformatted(root);
    VT_CHECK(!strcmp(before, after)); /* caller retains the exact sparse input */
    VT_CHECK_INT(vp_validate(root, vp_validate_default_cfg(), NULL), 0);
    free(before); free(after); cJSON_Delete(root);
    vp_profile_free(p, &vt_malloc_allocator); p = NULL;
    VT_CHECK_INT(compile("{" HEAD ",\"defaults\":\"none\",\"signals\":["
        "{\"id\":\"obd.speed\",\"src\":[{\"req\":\"010D\",\"len\":1,\"rpn\":[[\"A\",0]]}]}]}", &p, NULL), VP_COMPILE_OK);
    if (p) { VT_CHECK_INT(p->n_sig, 1); VT_CHECK_INT(p->n_req, 1); }
    vp_profile_free(p, &vt_malloc_allocator);
    p = NULL;
    VT_CHECK_INT(compile("{" HEAD ",\"drivetrain\":{\"fd\":3.5,\"tire_m\":0.315,\"gears\":[3.5]},"
        "\"signals\":[{\"id\":\"obd.gear\",\"derive\":\"ratio\"}]}", &p, NULL), VP_COMPILE_OK);
    if (p) {
        VT_CHECK_INT(p->n_sig, 11);
        VT_CHECK_INT(p->gear_count, 1);
        VT_CHECK(vp_profile_find_signal(p, "obd.rpm") >= 0);
        VT_CHECK(vp_profile_find_signal(p, "obd.speed") >= 0);
    }
    vp_profile_free(p, &vt_malloc_allocator);
    expect_invalid("{" HEAD ",\"signals\":["
        "{\"id\":\"obd.oil_temp\",\"rate\":\"every\",\"src\":[{\"req\":\"015C\",\"len\":1,\"rpn\":[[\"A\",0]]}]},"
        "{\"id\":\"obd.coolant_temp\",\"rate\":\"every\",\"src\":[{\"req\":\"0105\",\"len\":1,\"rpn\":[[\"A\",0]]}]}]}", "E_RATE");
    expect_invalid("{" HEAD ",\"defaults\":\"none\"}", "E_SCHEMA");
    expect_invalid("{" HEAD ",\"defaults\":\"invalid\"}", "E_SCHEMA");
    expect_invalid("{" HEAD ",\"defaults\":true}", "E_SCHEMA");
    expect_invalid("{" HEAD ",\"signals\":[{\"id\":\"obd.oil_temp\"}]}", "E_SCHEMA");
    expect_invalid("{" HEAD ",\"signals\":[{\"id\":\"obd.oil_temp\",\"disabled\":1}]}", "E_SCHEMA");
    expect_invalid("{" HEAD ",\"signals\":[{\"id\":\"obd.oil_temp\",\"disabled\":true,\"period_ms\":500}]}", "E_SCHEMA");
    expect_invalid("{" HEAD ",\"signals\":[{\"id\":\"obd.oil_temp\",\"disabled\":true},"
        "{\"id\":\"obd.oil_temp\",\"disabled\":true}]}", "E_SIGNAL");
    expect_invalid("{" HEAD ",\"drivetrain\":{\"fd\":3.5,\"tire_m\":0.315,\"gears\":[3.5]},"
        "\"signals\":[{\"id\":\"obd.rpm\",\"disabled\":true},{\"id\":\"obd.gear\",\"derive\":\"ratio\"}]}", "E_SCHEMA");
}

static void test_effective_limits(void)
{
    cJSON *root = cJSON_Parse(minimal), *signals = cJSON_AddArrayToObject(root, "signals");
    unsigned i, j;
    char *text;
    vp_profile_t *p = NULL;
    /* 24 explicit requests are legal; inheriting 10 more exceeds the cap of 32. */
    for (i = 0; i < 6; i++) {
        char id[32];
        cJSON *s = cJSON_CreateObject(), *sources;
        snprintf(id, sizeof(id), "veh.test_%u", i);
        cJSON_AddStringToObject(s, "id", id);
        sources = cJSON_AddArrayToObject(s, "src");
        for (j = 0; j < 4; j++) {
            char req[8];
            cJSON *src = cJSON_CreateObject();
            snprintf(req, sizeof(req), "22%04X", i * 4 + j);
            cJSON_AddStringToObject(src, "req", req);
            cJSON_AddNumberToObject(src, "len", 1);
            cJSON_AddItemToObject(src, "rpn", cJSON_Parse("[[\"A\",0]]"));
            cJSON_AddItemToArray(sources, src);
        }
        cJSON_AddItemToArray(signals, s);
    }
    text = cJSON_PrintUnformatted(root);
    expect_invalid(text, "E_LIMIT"); free(text);
    cJSON_AddStringToObject(root, "defaults", "none");
    VT_CHECK_INT(vp_compile(root, vp_validate_default_cfg(), &vt_malloc_allocator, &p, NULL), VP_COMPILE_OK);
    if (p) VT_CHECK_INT(p->n_req, 24);
    vp_profile_free(p, &vt_malloc_allocator);
    cJSON_Delete(root);
}

static size_t attempts, fail_at, live;
static void *json_alloc(size_t n)
{
    void *p;
    attempts++;
    if (fail_at && attempts == fail_at) return NULL;
    p = malloc(n);
    if (p) live++;
    return p;
}
static void json_free(void *p) { if (p) { live--; free(p); } }

static void test_expansion_oom(void)
{
    cJSON *root = cJSON_Parse(minimal);
    cJSON_Hooks hooks = { json_alloc, json_free };
    size_t total, i;
    vp_profile_t *p = NULL;
    attempts = fail_at = live = 0;
    cJSON_InitHooks(&hooks);
    VT_CHECK_INT(vp_compile(root, vp_validate_default_cfg(), &vt_malloc_allocator, &p, NULL), VP_COMPILE_OK);
    total = attempts;
    VT_CHECK_INT(live, 0);
    vp_profile_free(p, &vt_malloc_allocator);
    for (i = 1; i <= total; i++) {
        vp_issue_t store[4]; vp_issues_t issues;
        vp_issues_init(&issues, store, 4);
        attempts = live = 0; fail_at = i; p = NULL;
        VT_CHECK_INT(vp_compile(root, vp_validate_default_cfg(), &vt_malloc_allocator, &p, &issues), VP_COMPILE_ERR_NOMEM);
        VT_CHECK(p == NULL);
        VT_CHECK_INT(live, 0);
        VT_CHECK(issues.count && !strcmp(store[0].code, "E_NOMEM"));
    }
    cJSON_InitHooks(NULL);
    cJSON_Delete(root);
}

int main(void)
{
    VT_RUN(test_minimal);
    VT_RUN(test_override_and_disable);
    VT_RUN(test_policy_and_validation);
    VT_RUN(test_effective_limits);
    VT_RUN(test_expansion_oom);
    return VT_SUMMARY("test_defaults");
}
