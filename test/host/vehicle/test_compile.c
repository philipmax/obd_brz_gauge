/* Compiler / compiled-profile tests. */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "vp_profile.h"
#include "vt.h"
#include "vt_support.h"
#ifdef VT_HAVE_BUILTIN
#include "vp_builtin.h"
#endif

#ifndef VT_FIXTURE_DIR
#define VT_FIXTURE_DIR "fixtures"
#endif

static int s_allocs;
static int s_frees;
static int s_fail_alloc;
static char *s_full_text;
static vp_validate_cfg_t s_cfg;

static void *cnt_alloc(void *ctx, size_t n)
{
    (void)ctx;
    if (s_fail_alloc) return NULL;
    s_allocs++;
    return malloc(n);
}

static void cnt_free(void *ctx, void *p)
{
    (void)ctx;
    s_frees++;
    free(p);
}

static const vp_allocator_t s_alloc = { cnt_alloc, cnt_free, NULL };

static long reg_id(const char *key)
{
    size_t i;
    for (i = 0; i < s_cfg.n_registry; i++) {
        if (!strcmp(s_cfg.registry[i].key, key)) return s_cfg.registry[i].num_id;
    }
    return -1;
}

static vp_profile_t *compile_text(const char *text, vp_compile_err_t *err)
{
    vp_profile_t *p = NULL;
    vp_issue_t store[16];
    vp_issues_t l;
    vp_compile_err_t e;

    vp_issues_init(&l, store, 16);
    e = vp_compile_json(text, strlen(text), &s_cfg, &s_alloc, &p, &l);
    if (err) *err = e;
    if (e != VP_COMPILE_OK && !err) vt_print_issues(&l, "  ");
    return p;
}

static void test_layout(void)
{
    vp_profile_t *p;
    const vp_signal_t *sig;
    const vp_source_t *src;
    const vp_request_t *req;
    const vp_can_rule_t *can;
    const vp_at_cmd_t *init;

    s_allocs = s_frees = 0;
    p = compile_text(s_full_text, NULL);
    VT_CHECK(p != NULL);
    if (!p) return;
    VT_CHECK_INT(s_allocs, 1);   /* single allocation */
    VT_CHECK(vp_profile_check(p, p->total_size));
    VT_CHECK_INT(p->total_size % 4, 0);

    VT_CHECK(!strcmp(p->id, "test.full"));
    VT_CHECK(!strcmp(p->name, "Test Full"));
    VT_CHECK_INT(p->rev, 3);
    VT_CHECK_INT(p->requires, VP_REQ_ATMA | VP_REQ_ATCEA);
    VT_CHECK_INT(p->proto, 6);
    VT_CHECK_INT(p->hdr, 0x7E0);
    VT_CHECK_INT(p->hdr_functional, 0);
    VT_CHECK_INT(p->atst, 10);
    VT_CHECK_INT(p->gap_ms, 1);
    VT_CHECK_INT(p->gear_count, 6);
    VT_CHECK_FLT(p->gears[0], 3.626, 1e-6);
    VT_CHECK_FLT(p->final_drive, 4.1, 1e-6);
    VT_CHECK_FLT(p->speed_scale, 1.0, 0);
    VT_CHECK_INT(p->mon_mode, VP_MON_PROBE);
    VT_CHECK_INT(p->mon_every_ms, 500);
    VT_CHECK_INT(p->mon_window_ms, 120);
    VT_CHECK_INT(p->mon_stale_ms, 15000);
    VT_CHECK_INT(p->min_req_ms, 10);

    VT_CHECK_INT(p->n_init, 2);
    VT_CHECK_INT(p->n_sig, 10);
    VT_CHECK_INT(p->n_src, 12);
    VT_CHECK_INT(p->n_can, 2);
    VT_CHECK_INT(p->n_req, 9);   /* "0105" shared by two signals */
    VT_CHECK_INT(p->n_lut, 1);
    VT_CHECK_INT(p->n_lut_entry, 3);

    init = vp_profile_init(p);
    VT_CHECK(!strcmp(init[0].cmd, "ATCAF1"));
    VT_CHECK(!strcmp(init[1].cmd, "ATSP6"));

    sig = vp_profile_signals(p);
    src = vp_profile_sources(p);
    req = vp_profile_requests(p);
    can = vp_profile_can_rules(p);

    VT_CHECK(!strcmp(sig[0].key, "obd.rpm"));
    VT_CHECK_INT(sig[0].num_id, reg_id("obd.rpm"));
    VT_CHECK_INT(sig[0].rate, VP_RATE_EVERY);
    VT_CHECK_INT(sig[1].flags, VP_SIG_F_ZERO_LE | VP_SIG_F_ZERO_RPM);
    VT_CHECK_INT(sig[1].zero_if_rpm_lt, 800);
    VT_CHECK_FLT(sig[1].zero_le, 2, 0);
    VT_CHECK_INT(sig[3].flags, VP_SIG_F_VALID);
    VT_CHECK_INT(sig[3].filter, VP_FILTER_TEMP_HYST);
    VT_CHECK_FLT(sig[3].valid_lo, -20, 0);
    VT_CHECK_INT(sig[5].flags, VP_SIG_F_CLAMP);
    VT_CHECK_FLT(sig[5].clamp_hi, 20, 0);
    VT_CHECK_INT(sig[6].num_id, reg_id("obd.gear"));
    VT_CHECK_INT(sig[6].fallback, VP_DERIVE_RATIO);
    VT_CHECK_INT(sig[7].num_id, VP_VEH_NUM_ID_BASE);
    VT_CHECK_INT(sig[7].flags, VP_SIG_F_VEH);
    VT_CHECK(!strcmp(sig[7].name, "Trans temp"));
    VT_CHECK(!strcmp(sig[7].unit, "C"));
    VT_CHECK_INT(sig[7].rate, VP_RATE_SLOW);
    VT_CHECK_INT(sig[7].slow_div, 8);
    VT_CHECK_INT(sig[8].num_id, VP_VEH_NUM_ID_BASE + 1);
    VT_CHECK_INT(sig[8].rate, VP_RATE_ON_DEMAND);
    VT_CHECK_INT(sig[8].period_ms, 500);
    VT_CHECK_INT(sig[9].num_id, VP_VEH_NUM_ID_BASE + 2);

    /* coolant: can source then request; veh.coolant_raw reuses the request */
    VT_CHECK_INT(sig[2].src_first, 2);
    VT_CHECK_INT(sig[2].src_count, 2);
    VT_CHECK_INT(src[2].kind, VP_SRC_CAN);
    VT_CHECK_INT(src[3].kind, VP_SRC_REQ);
    VT_CHECK_INT(src[sig[8].src_first].ref, src[3].ref);
    VT_CHECK_INT(can[0].can_id, 0x360);
    VT_CHECK_INT(can[0].src, 2);
    VT_CHECK_INT(can[0].sig, 2);
    VT_CHECK_INT(src[2].len, 8);
    VT_CHECK_INT(src[2].raw_start, 24);
    VT_CHECK_INT(src[2].raw_bits, 8);

    /* oil temp 21 01 tail byte with hold */
    {
        const vp_source_t *o = &src[sig[3].src_first + 1];
        const vp_request_t *r = &req[o->ref];
        VT_CHECK_INT(o->flags, VP_SRC_F_FROM_END | VP_SRC_F_HOLD | VP_SRC_F_VALID);
        VT_CHECK_INT(o->byte_from_end, 5);
        VT_CHECK_INT(o->len, 5);
        VT_CHECK_FLT(o->hold_delta, 8, 0);
        VT_CHECK_INT(o->hold_max, 30);
        VT_CHECK_INT(o->hold_gap_ms, 3000);
        VT_CHECK_FLT(o->valid_lo, -10, 0);
        VT_CHECK_INT(r->flags, VP_REQ_F_MULTI);
        VT_CHECK_INT(r->tx_len, 2);
        VT_CHECK_INT(r->expect_len, 2);
        VT_CHECK_INT(r->expect[0], 0x61);
        VT_CHECK_INT(r->expect[1], 0x01);
        VT_CHECK_INT(o->fail_after, VP_DEFAULT_FAIL_AFTER);
    }
    /* BMW-style gear: raw ext-addressed frame */
    {
        const vp_request_t *r = &req[src[sig[6].src_first].ref];
        static const uint8_t tx[5] = { 0x18, 0x03, 0x22, 0xDA, 0x2E };
        VT_CHECK_INT(r->flags, VP_REQ_F_RAW | VP_REQ_F_RX);
        VT_CHECK_INT(r->hdr, 0x6F1);
        VT_CHECK_INT(r->rx, 0x618);
        VT_CHECK_INT(r->service, 0x22);
        VT_CHECK_INT(r->tx_len, 5);
        VT_CHECK(memcmp(r->tx, tx, 5) == 0);
        VT_CHECK_INT(r->expect_len, 3);
        VT_CHECK_INT(r->expect[0], 0x62);
    }
    /* extended addressing via CEA */
    {
        const vp_request_t *r = &req[src[sig[7].src_first].ref];
        VT_CHECK_INT(r->flags, VP_REQ_F_CEA);
        VT_CHECK_INT(r->cea, 0x10);
        VT_CHECK_INT(r->hdr, 0x7E1);
        VT_CHECK_INT(r->service, 0x22);
        VT_CHECK_INT(r->expect_len, 3);
    }
    vp_profile_free(p, &s_alloc);
    VT_CHECK_INT(s_frees, 1);
}

static void test_eval(void)
{
    vp_profile_t *p = compile_text(s_full_text, NULL);
    const vp_signal_t *sig;
    float v = 0;
    uint8_t resp[39];

    VT_CHECK(p != NULL);
    if (!p) return;
    sig = vp_profile_signals(p);
    {
        const uint8_t d[2] = { 0x1A, 0xF8 };
        VT_CHECK_INT(vp_profile_eval_source(p, sig[0].src_first, d, 2, &v), VP_RPN_OK);
        VT_CHECK_FLT(v, 1727.5, 0);
        VT_CHECK_INT(vp_profile_eval_source(p, sig[0].src_first, d, 1, &v), VP_RPN_ERR_DATA);
    }
    {
        const uint8_t frame[8] = { 0, 0, 0x82, 0x7A, 0, 0, 0, 0 };
        VT_CHECK_INT(vp_profile_eval_source(p, sig[2].src_first, frame, 8, &v), VP_RPN_OK);
        VT_CHECK_FLT(v, 0x7A - 40, 0);
        VT_CHECK_INT(vp_profile_eval_source(p, sig[3].src_first, frame, 8, &v), VP_RPN_OK);
        VT_CHECK_FLT(v, 0x82 - 40, 0);
    }
    /* 21 01 response: oil temperature 5th byte from the end (38 or 39 data bytes) */
    memset(resp, 0x11, sizeof(resp));
    resp[33] = 0x82;
    resp[34] = 0x83;
    VT_CHECK_INT(vp_profile_eval_source(p, sig[3].src_first + 1, resp, 38, &v), VP_RPN_OK);
    VT_CHECK_FLT(v, 0x82 - 40, 0);
    VT_CHECK_INT(vp_profile_eval_source(p, sig[3].src_first + 1, resp, 39, &v), VP_RPN_OK);
    VT_CHECK_FLT(v, 0x83 - 40, 0);
    VT_CHECK_INT(vp_profile_eval_source(p, sig[3].src_first + 1, resp, 4, &v), VP_RPN_ERR_DATA);
    resp[33] = 0xFF;   /* 215 C: outside the source's valid range */
    VT_CHECK_INT(vp_profile_eval_source(p, sig[3].src_first + 1, resp, 38, &v), VP_RPN_ERR_RANGE);
    /* gear through the LUT */
    {
        uint8_t g[1] = { 3 };
        VT_CHECK_INT(vp_profile_eval_source(p, sig[6].src_first, g, 1, &v), VP_RPN_OK);
        VT_CHECK_FLT(v, 1, 0);
        g[0] = 7;
        VT_CHECK_INT(vp_profile_eval_source(p, sig[6].src_first, g, 1, &v), VP_RPN_OK);
        VT_CHECK_FLT(v, -1, 0);
        g[0] = 9;
        VT_CHECK_INT(vp_profile_eval_source(p, sig[6].src_first, g, 1, &v), VP_RPN_ERR_LUT_MISS);
    }
    {
        const uint8_t d[2] = { 0x13, 0x88 };
        VT_CHECK_INT(vp_profile_eval_source(p, sig[5].src_first, d, 2, &v), VP_RPN_OK);
        VT_CHECK_FLT(v, 5.0, 1e-6);
    }
    {
        const uint8_t d[4] = { 0x83, 0, 0, 0 };
        VT_CHECK_INT(vp_profile_eval_source(p, sig[9].src_first, d, 4, &v), VP_RPN_OK);
        VT_CHECK_FLT(v, 3, 0);
    }
    VT_CHECK_INT(vp_profile_eval_source(p, 200, resp, 4, &v), VP_RPN_ERR_ARG);
    VT_CHECK_INT(vp_profile_find_signal(p, "veh.mil_bits"), 9);
    VT_CHECK_INT(vp_profile_find_signal(p, "obd.boost"), -1);
    vp_profile_free(p, &s_alloc);
}

static uint8_t *dup_blob(const vp_profile_t *p)
{
    uint8_t *b = (uint8_t *)malloc(p->total_size);
    memcpy(b, p, p->total_size);
    return b;
}

static void reseal(uint8_t *b)
{
    vp_profile_t *q = (vp_profile_t *)(void *)b;
    q->crc32 = 0;
    q->crc32 = vp_crc32(0, b, q->total_size);
}

static void test_check(void)
{
    vp_profile_t *p = compile_text(s_full_text, NULL);
    uint8_t *b;
    vp_profile_t *q;

    VT_CHECK(p != NULL);
    if (!p) return;
    VT_CHECK(vp_profile_check(p, p->total_size));
    VT_CHECK(vp_profile_check(p, p->total_size + 100));
    VT_CHECK(!vp_profile_check(p, p->total_size - 1));
    VT_CHECK(!vp_profile_check(NULL, 1000));

    b = dup_blob(p);
    b[40] ^= 1;   /* inside id: CRC mismatch */
    VT_CHECK(!vp_profile_check(b, p->total_size));
    reseal(b);
    VT_CHECK(vp_profile_check(b, p->total_size));
    free(b);

    b = dup_blob(p);
    q = (vp_profile_t *)(void *)b;
    ((vp_rpn_insn_t *)(void *)(b + q->off_code))[0].op = 0x55;
    reseal(b);
    VT_CHECK(!vp_profile_check(b, p->total_size));
    free(b);

    b = dup_blob(p);
    q = (vp_profile_t *)(void *)b;
    ((vp_rpn_insn_t *)(void *)(b + q->off_code))[0].u.arg = 40;   /* byte index beyond len */
    reseal(b);
    VT_CHECK(!vp_profile_check(b, p->total_size));
    free(b);

    b = dup_blob(p);
    q = (vp_profile_t *)(void *)b;
    q->n_sig = 49;
    reseal(b);
    VT_CHECK(!vp_profile_check(b, p->total_size));
    free(b);

    b = dup_blob(p);
    q = (vp_profile_t *)(void *)b;
    ((vp_source_t *)(void *)(b + q->off_src))[0].ref = 200;
    reseal(b);
    VT_CHECK(!vp_profile_check(b, p->total_size));
    free(b);

    b = dup_blob(p);
    q = (vp_profile_t *)(void *)b;
    {
        vp_request_t *r = (vp_request_t *)(void *)(b + q->off_req);
        r[0].tx[0] = 0x04;   /* forbidden service smuggled into a blob */
        r[0].service = 0x04;
        r[0].tx_len = 1;
    }
    reseal(b);
    VT_CHECK(!vp_profile_check(b, p->total_size));
    free(b);

    b = dup_blob(p);
    q = (vp_profile_t *)(void *)b;
    {
        vp_request_t *r = (vp_request_t *)(void *)(b + q->off_req);
        r[0].hdr = 0x7DF;   /* functional header without the flag */
    }
    reseal(b);
    VT_CHECK(!vp_profile_check(b, p->total_size));
    free(b);

    b = dup_blob(p);
    q = (vp_profile_t *)(void *)b;
    q->off_sig = q->total_size - 8;   /* table past the end */
    reseal(b);
    VT_CHECK(!vp_profile_check(b, p->total_size));
    free(b);

    b = dup_blob(p);
    q = (vp_profile_t *)(void *)b;
    strcpy(((vp_at_cmd_t *)(void *)(b + q->off_init))[0].cmd, "ATZ");
    reseal(b);
    VT_CHECK(!vp_profile_check(b, p->total_size));
    free(b);

    /* misaligned copy */
    b = (uint8_t *)malloc(p->total_size + 4);
    memcpy(b + 1, p, p->total_size);
    VT_CHECK(!vp_profile_check(b + 1, p->total_size));
    free(b);

    vp_profile_free(p, &s_alloc);
}

static void test_errors(void)
{
    vp_profile_t *p = (vp_profile_t *)(void *)&s_cfg;   /* must be reset to NULL */
    vp_compile_err_t e;
    vp_issue_t store[8];
    vp_issues_t l;
    cJSON *root;

    s_allocs = 0;
    root = cJSON_Parse(s_full_text);
    cJSON_ReplaceItemInObjectCaseSensitive(root, "rev", cJSON_CreateNumber(0));
    vp_issues_init(&l, store, 8);
    e = vp_compile(root, &s_cfg, &s_alloc, &p, &l);
    VT_CHECK_INT(e, VP_COMPILE_ERR_INVALID);
    VT_CHECK(p == NULL);
    VT_CHECK_INT(s_allocs, 0);
    VT_CHECK(l.count > 0 && !strcmp(l.items[0].code, "E_SCHEMA"));
    cJSON_Delete(root);

    VT_CHECK(compile_text("{not json", &e) == NULL);
    VT_CHECK_INT(e, VP_COMPILE_ERR_PARSE);

    s_fail_alloc = 1;
    VT_CHECK(compile_text(s_full_text, &e) == NULL);
    VT_CHECK_INT(e, VP_COMPILE_ERR_NOMEM);
    s_fail_alloc = 0;

    VT_CHECK_INT(vp_compile(NULL, &s_cfg, &s_alloc, &p, NULL), VP_COMPILE_ERR_ARG);
    VT_CHECK_INT(vp_compile_json(s_full_text, strlen(s_full_text), &s_cfg, NULL, &p, NULL), VP_COMPILE_ERR_ARG);
    {
        vp_validate_cfg_t small = s_cfg;
        small.max_json_bytes = 100;
        vp_issues_init(&l, store, 8);
        VT_CHECK_INT(vp_compile_json(s_full_text, strlen(s_full_text), &small, &s_alloc, &p, &l),
                     VP_COMPILE_ERR_INVALID);
        VT_CHECK(l.count > 0 && !strcmp(l.items[0].code, "E_SIZE"));
    }
}

static void test_deterministic(void)
{
    vp_profile_t *a = compile_text(s_full_text, NULL);
    vp_profile_t *b = compile_text(s_full_text, NULL);
    VT_CHECK(a && b);
    if (a && b) {
        VT_CHECK_INT(a->total_size, b->total_size);
        VT_CHECK(memcmp(a, b, a->total_size) == 0);
    }
    vp_profile_free(a, &s_alloc);
    vp_profile_free(b, &s_alloc);
}

static void test_builtin(const char *generic_path)
{
#ifdef VT_HAVE_BUILTIN
    const vp_profile_t *bp = vp_builtin_generic();
    char *text;
    vp_profile_t *p;

    VT_CHECK(vp_profile_check(vp_builtin_generic_blob, vp_builtin_generic_blob_size));
    VT_CHECK(!strcmp(bp->id, "obd2.generic"));
    VT_CHECK_INT(bp->total_size, vp_builtin_generic_blob_size);
    VT_CHECK(vp_profile_find_signal(bp, "obd.rpm") >= 0);

    /* the embedded JSON compiles to the embedded blob */
    p = compile_text(vp_builtin_generic_json, NULL);
    VT_CHECK(p != NULL);
    if (p) {
        VT_CHECK_INT(p->total_size, vp_builtin_generic_blob_size);
        VT_CHECK_MSG(memcmp(p, vp_builtin_generic_blob, p->total_size) == 0,
                     "vp_builtin_generic.c is stale: run tools/vehicles/gen_builtin.py");
        vp_profile_free(p, &s_alloc);
    }
    /* and the catalog file is what was embedded */
    text = generic_path ? vt_read_file(generic_path, NULL) : NULL;
    VT_CHECK_MSG(text != NULL, "cannot read %s", generic_path ? generic_path : "(null)");
    if (text) {
        p = compile_text(text, NULL);
        VT_CHECK(p != NULL);
        if (p) {
            VT_CHECK_MSG(p->total_size == vp_builtin_generic_blob_size &&
                             memcmp(p, vp_builtin_generic_blob, p->total_size) == 0,
                         "%s differs from vp_builtin_generic.c: run tools/vehicles/gen_builtin.py", generic_path);
            vp_profile_free(p, &s_alloc);
        }
        free(text);
    }
#else
    (void)generic_path;
    printf("  (vp_builtin_generic.c not generated yet, skipped)\n");
#endif
}

int main(int argc, char **argv)
{
    const char *generic_path = NULL;
    vt_registry_t reg = { NULL, 0, NULL };
    int i;

    s_cfg.registry = vt_default_registry;
    s_cfg.n_registry = vt_default_registry_count;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--signals") && i + 1 < argc) {
            if (vt_registry_load(argv[++i], &reg) != 0) return 2;
            s_cfg.registry = reg.defs;
            s_cfg.n_registry = reg.n;
        } else {
            generic_path = argv[i];
        }
    }
    s_full_text = vt_read_file(VT_FIXTURE_DIR "/good_full.json", NULL);
    if (!s_full_text) {
        fprintf(stderr, "cannot read fixtures from %s\n", VT_FIXTURE_DIR);
        return 2;
    }

    VT_RUN(test_layout);
    VT_RUN(test_eval);
    VT_RUN(test_check);
    VT_RUN(test_errors);
    VT_RUN(test_deterministic);
    test_builtin(generic_path);
    printf("%s test_builtin\n", vt_failures ? "?" : "PASS");
    free(s_full_text);
    vt_registry_free(&reg);
    return VT_SUMMARY("test_compile");
}
