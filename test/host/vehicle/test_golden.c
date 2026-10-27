/*
 * test_golden — compare every compiled catalog profile with its legacy
 * golden fixture (fixtures/golden/<id>.json, written by
 * tools/vehicles/legacy_golden.py; format in fixtures/golden/README.md).
 *
 *   test_golden [catalog_dir] [golden_dir]
 *
 * Checked per profile, compiled with vp_validate_default_cfg():
 *  - init: the engine bring-up ATZ ATE0 ATH1 ATS0 ATM0 ATAT1 ATAL
 *    ATST<atst> ATSP<proto> ATSH<hdr> followed by the profile's bus.init;
 *  - requests: one entry per request source in signal/source order with
 *    sig, src, hdr, tx, class (signal rate) and the raw/rx/multi options;
 *  - gap_ms, requires, monitor, CAN rules (as a set) and drivetrain.
 * Every catalog profile with origin.legacy_idx must have a fixture and
 * every fixture a catalog profile.
 */
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "vp_profile.h"
#include "vt.h"
#include "vt_support.h"

#define MAX_REQS  48
#define MAX_INIT  (9 + VP_MAX_INIT_CMDS)
#define MAX_FILES 128

typedef struct {
    char sig[32];
    int  src;
    char hdr[12];
    char tx[20];
    char cls[12];
    char rx[12];
    bool raw;
    bool multi;
} greq_t;

typedef struct {
    char sig[32];
    char id[12];
    int  bit;
    int  bits;
    bool used;
} gcan_t;

static const char *s_catalog_dir = VT_CATALOG_DIR;
static const char *s_golden_dir = VT_FIXTURE_DIR "/golden";
static vp_validate_cfg_t s_cfg;

static void fmt_id(char *out, size_t n, uint32_t id)
{
    if (id & VP_ID_29BIT) snprintf(out, n, "%08X", (unsigned)(id & ~VP_ID_29BIT));
    else snprintf(out, n, "%03X", (unsigned)id);
}

static void fmt_hex(char *out, size_t n, const uint8_t *b, size_t len)
{
    size_t i;
    out[0] = '\0';
    for (i = 0; i < len && 2 * i + 2 < n; i++) snprintf(out + 2 * i, n - 2 * i, "%02X", b[i]);
}

static const char *rate_class(uint8_t rate)
{
    switch (rate) {
    case VP_RATE_EVERY:     return "fast";
    case VP_RATE_SLOW:      return "slow";
    case VP_RATE_ON_DEMAND: return "on_demand";
    default:                return "round";
    }
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static cJSON *load_json(const char *path)
{
    size_t len = 0;
    char *text = vt_read_file(path, &len);
    cJSON *j;
    if (!text) return NULL;
    j = cJSON_ParseWithLength(text, len);
    free(text);
    return j;
}

/* ---- compiled side ---- */

static int compiled_init(const vp_profile_t *p, char out[][24])
{
    const vp_at_cmd_t *init = vp_profile_init(p);
    char hdr[12];
    int n = 0;
    uint16_t i;
    static const char *const fixed[] = { "ATZ", "ATE0", "ATH1", "ATS0", "ATM0", "ATAT1", "ATAL" };

    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) snprintf(out[n++], 24, "%s", fixed[i]);
    snprintf(out[n++], 24, "ATST%02X", (unsigned)p->atst);
    snprintf(out[n++], 24, "ATSP%X", (unsigned)p->proto);
    fmt_id(hdr, sizeof(hdr), p->hdr);
    snprintf(out[n++], 24, "ATSH%s", hdr);
    for (i = 0; i < p->n_init && n < MAX_INIT; i++) snprintf(out[n++], 24, "%s", init[i].cmd);
    return n;
}

static int compiled_requests(const vp_profile_t *p, greq_t *out)
{
    const vp_signal_t *sig = vp_profile_signals(p);
    const vp_source_t *src = vp_profile_sources(p);
    const vp_request_t *req = vp_profile_requests(p);
    int n = 0;
    uint16_t s, k;

    for (s = 0; s < p->n_sig; s++) {
        for (k = 0; k < sig[s].src_count; k++) {
            const vp_source_t *o = &src[sig[s].src_first + k];
            const vp_request_t *r;
            greq_t *g;
            if (o->kind != VP_SRC_REQ || n >= MAX_REQS) continue;
            r = &req[o->ref];
            g = &out[n++];
            memset(g, 0, sizeof(*g));
            snprintf(g->sig, sizeof(g->sig), "%s", sig[s].key);
            g->src = k;
            fmt_id(g->hdr, sizeof(g->hdr), r->hdr);
            fmt_hex(g->tx, sizeof(g->tx), r->tx, r->tx_len);
            snprintf(g->cls, sizeof(g->cls), "%s", rate_class(sig[s].rate));
            if (r->flags & VP_REQ_F_RX) fmt_id(g->rx, sizeof(g->rx), r->rx);
            g->raw = (r->flags & VP_REQ_F_RAW) != 0;
            g->multi = (r->flags & VP_REQ_F_MULTI) != 0;
        }
    }
    return n;
}

static int compiled_can(const vp_profile_t *p, gcan_t *out)
{
    const vp_can_rule_t *can = vp_profile_can_rules(p);
    const vp_source_t *src = vp_profile_sources(p);
    const vp_signal_t *sig = vp_profile_signals(p);
    int n = 0;
    uint16_t i;

    for (i = 0; i < p->n_can && n < MAX_REQS; i++) {
        gcan_t *g = &out[n++];
        memset(g, 0, sizeof(*g));
        snprintf(g->sig, sizeof(g->sig), "%s", sig[can[i].sig].key);
        fmt_id(g->id, sizeof(g->id), can[i].can_id);
        g->bit = src[can[i].src].raw_start;
        g->bits = src[can[i].src].raw_bits;
    }
    return n;
}

/* ---- golden side ---- */

static int golden_requests(const cJSON *arr, greq_t *out)
{
    const cJSON *it;
    int n = 0;

    cJSON_ArrayForEach(it, arr) {
        greq_t *g;
        if (n >= MAX_REQS) break;
        g = &out[n++];
        memset(g, 0, sizeof(*g));
        snprintf(g->sig, sizeof(g->sig), "%s", jstr(it, "sig") ? jstr(it, "sig") : "");
        g->src = (int)jnum(it, "src", -1);
        snprintf(g->hdr, sizeof(g->hdr), "%s", jstr(it, "hdr") ? jstr(it, "hdr") : "");
        snprintf(g->tx, sizeof(g->tx), "%s", jstr(it, "tx") ? jstr(it, "tx") : "");
        snprintf(g->cls, sizeof(g->cls), "%s", jstr(it, "class") ? jstr(it, "class") : "");
        snprintf(g->rx, sizeof(g->rx), "%s", jstr(it, "rx") ? jstr(it, "rx") : "");
        g->raw = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "raw"));
        g->multi = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "multi"));
    }
    return n;
}

static void print_req(const char *tag, const greq_t *g)
{
    fprintf(stderr, "      %s %-20s src %d  %-8s %-12s %-6s%s%s%s%s\n", tag, g->sig, g->src, g->hdr, g->tx,
            g->cls, g->raw ? " raw" : "", g->multi ? " multi" : "", g->rx[0] ? " rx=" : "", g->rx);
}

static bool req_eq(const greq_t *a, const greq_t *b)
{
    return !strcmp(a->sig, b->sig) && a->src == b->src && !strcmp(a->hdr, b->hdr) && !strcmp(a->tx, b->tx) &&
           !strcmp(a->cls, b->cls) && !strcmp(a->rx, b->rx) && a->raw == b->raw && a->multi == b->multi;
}

/* ---- one fixture ---- */

static void check_one(const char *golden_path)
{
    cJSON *g = load_json(golden_path);
    cJSON *cat = NULL;
    const char *id = g ? jstr(g, "id") : NULL;
    char path[512];
    size_t len = 0;
    char *text = NULL;
    vp_profile_t *p = NULL;
    vp_issue_t store[32];
    vp_issues_t issues;
    int before = vt_failures;

    VT_CHECK_MSG(id != NULL, "%s: not a golden fixture", golden_path);
    if (!id) goto out;

    snprintf(path, sizeof(path), "%s/%s.vehicle.json", s_catalog_dir, id);
    text = vt_read_file(path, &len);
    VT_CHECK_MSG(text != NULL, "%s: no catalog profile %s", id, path);
    if (!text) goto out;
    vp_issues_init(&issues, store, 32);
    VT_CHECK_INT(vp_compile_json(text, len, &s_cfg, &vt_malloc_allocator, &p, &issues), VP_COMPILE_OK);
    if (!p) {
        vt_print_issues(&issues, "    ");
        goto out;
    }

    /* legacy index recorded on both sides */
    cat = cJSON_ParseWithLength(text, len);
    {
        const cJSON *origin = cJSON_GetObjectItemCaseSensitive(cat, "origin");
        const cJSON *legacy = cJSON_GetObjectItemCaseSensitive(g, "legacy");
        VT_CHECK_MSG(jnum(origin, "legacy_idx", -1) == jnum(legacy, "idx", -2), "%s: legacy index differs", id);
    }

    /* init */
    {
        char init[MAX_INIT][24];
        int n = compiled_init(p, init), i = 0;
        const cJSON *ga = cJSON_GetObjectItemCaseSensitive(g, "init"), *it;
        bool ok = cJSON_GetArraySize(ga) == n;
        cJSON_ArrayForEach(it, ga) {
            if (i >= n || !cJSON_IsString(it) || strcmp(it->valuestring, init[i]) != 0) ok = false;
            i++;
        }
        VT_CHECK_MSG(ok, "%s: init differs", id);
        if (!ok) {
            fprintf(stderr, "    compiled:");
            for (i = 0; i < n; i++) fprintf(stderr, " %s", init[i]);
            fprintf(stderr, "\n");
        }
    }

    /* requests, in order */
    {
        greq_t want[MAX_REQS], got[MAX_REQS];
        int nw = golden_requests(cJSON_GetObjectItemCaseSensitive(g, "requests"), want);
        int ng = compiled_requests(p, got), i;
        /* Preserve the historical fixture, explicitly allow only the new
         * inherited MAP signal. Its formula is covered by pressure tests. */
        bool legacy_boost = false;
        for (i = 0; i < nw; i++) if (!strcmp(want[i].sig, "obd.boost")) legacy_boost = true;
        if (!legacy_boost) {
            for (i = 0; i < ng; i++) if (!strcmp(got[i].sig, "obd.boost")) {
                VT_CHECK(!strcmp(got[i].tx, "010B") && !strcmp(got[i].cls, "round"));
                memmove(&got[i], &got[i + 1], (size_t)(ng - i - 1) * sizeof(got[0]));
                ng--; i--;
            }
        }
        bool ok = nw == ng;
        for (i = 0; ok && i < nw; i++) ok = req_eq(&want[i], &got[i]);
        VT_CHECK_MSG(ok, "%s: requests differ (golden %d, compiled %d)", id, nw, ng);
        if (!ok) {
            for (i = 0; i < nw || i < ng; i++) {
                if (i < nw && i < ng && req_eq(&want[i], &got[i])) continue;
                if (i < nw) print_req("golden  ", &want[i]);
                if (i < ng) print_req("compiled", &got[i]);
            }
        }
    }

    VT_CHECK_MSG(p->gap_ms == (uint16_t)jnum(g, "gap_ms", -1), "%s: gap_ms %u", id, (unsigned)p->gap_ms);

    /* requires / monitor */
    {
        const cJSON *rq = cJSON_GetObjectItemCaseSensitive(g, "requires"), *it;
        uint8_t want = 0;
        const cJSON *mon = cJSON_GetObjectItemCaseSensitive(g, "monitor");
        cJSON_ArrayForEach(it, rq) {
            if (cJSON_IsString(it) && !strcmp(it->valuestring, "ATMA")) want |= VP_REQ_ATMA;
            if (cJSON_IsString(it) && !strcmp(it->valuestring, "ATCEA")) want |= VP_REQ_ATCEA;
        }
        VT_CHECK_MSG(p->requires == want, "%s: requires 0x%02X", id, (unsigned)p->requires);
        if (cJSON_IsObject(mon)) {
            const char *mode = jstr(mon, "mode");
            VT_CHECK_MSG(p->mon_mode == (mode && !strcmp(mode, "continuous") ? VP_MON_CONTINUOUS : VP_MON_PROBE),
                         "%s: monitor mode %u", id, (unsigned)p->mon_mode);
            VT_CHECK_INT(p->mon_every_ms, jnum(mon, "every_ms", -1));
            VT_CHECK_INT(p->mon_window_ms, jnum(mon, "window_ms", -1));
            VT_CHECK_INT(p->mon_stale_ms, jnum(mon, "stale_ms", -1));
        } else {
            VT_CHECK_MSG(p->mon_mode == VP_MON_NONE, "%s: unexpected monitor", id);
        }
    }

    /* CAN rules, order-independent */
    {
        gcan_t got[MAX_REQS];
        int ng = compiled_can(p, got), nw = 0, i;
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(g, "can")) {
            const char *s = jstr(it, "sig"), *cid = jstr(it, "id");
            bool found = false;
            nw++;
            for (i = 0; i < ng && !found; i++) {
                if (!got[i].used && s && cid && !strcmp(got[i].sig, s) && !strcmp(got[i].id, cid) &&
                    got[i].bit == (int)jnum(it, "bit", -1) && got[i].bits == (int)jnum(it, "bits", -1)) {
                    got[i].used = found = true;
                }
            }
            VT_CHECK_MSG(found, "%s: CAN rule %s %s not compiled", id, s ? s : "?", cid ? cid : "?");
        }
        VT_CHECK_MSG(nw == ng, "%s: %d CAN rules, golden %d", id, ng, nw);
    }

    /* drivetrain */
    {
        const cJSON *dt = cJSON_GetObjectItemCaseSensitive(g, "drivetrain");
        const cJSON *gears = cJSON_GetObjectItemCaseSensitive(dt, "gears"), *it;
        int i = 0;
        VT_CHECK_FLT(p->final_drive, jnum(dt, "fd", -1), 1e-5);
        VT_CHECK_FLT(p->tire_m, jnum(dt, "tire_m", -1), 1e-5);
        VT_CHECK_FLT(p->gear_tol, jnum(dt, "tol", -1), 1e-5);
        VT_CHECK_MSG(p->gear_count == cJSON_GetArraySize(gears), "%s: %u gears", id, (unsigned)p->gear_count);
        cJSON_ArrayForEach(it, gears) {
            if (i < p->gear_count) VT_CHECK_FLT(p->gears[i], it->valuedouble, 1e-5);
            i++;
        }
    }

out:
    printf("%s %s\n", vt_failures == before ? "PASS" : "FAIL", id ? id : golden_path);
    if (p) vp_profile_free(p, &vt_malloc_allocator);
    cJSON_Delete(cat);
    cJSON_Delete(g);
    free(text);
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Collect "<dir>/<name>" for every name ending in suffix, sorted. */
static int list_dir(const char *dir, const char *suffix, char **names)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    if (!d) return -1;
    while ((e = readdir(d)) != NULL && n < MAX_FILES) {
        if (e->d_name[0] != '.' && ends_with(e->d_name, suffix)) {
            size_t sz = strlen(dir) + strlen(e->d_name) + 2;
            names[n] = (char *)malloc(sz);
            if (!names[n]) break;
            snprintf(names[n], sz, "%s/%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_str);
    return n;
}

/* Every catalog profile with a legacy origin has a fixture. */
static void check_coverage(void)
{
    char *names[MAX_FILES];
    int n = list_dir(s_catalog_dir, ".vehicle.json", names), i;

    VT_CHECK_MSG(n > 0, "no catalog profiles in %s", s_catalog_dir);
    for (i = 0; i < n; i++) {
        cJSON *j = load_json(names[i]);
        const cJSON *origin = cJSON_GetObjectItemCaseSensitive(j, "origin");
        const char *id = jstr(j, "id");
        if (id && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(origin, "legacy_idx"))) {
            char path[512];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s.json", s_golden_dir, id);
            VT_CHECK_MSG(stat(path, &st) == 0, "%s has a legacy origin but no golden fixture %s", id, path);
        }
        cJSON_Delete(j);
        free(names[i]);
    }
}

int main(int argc, char **argv)
{
    char *names[MAX_FILES];
    int n, i;

    if (argc > 1) s_catalog_dir = argv[1];
    if (argc > 2) s_golden_dir = argv[2];
    s_cfg = *vp_validate_default_cfg();

    n = list_dir(s_golden_dir, ".json", names);
    VT_CHECK_MSG(n > 0, "no golden fixtures in %s", s_golden_dir);
    for (i = 0; i < n; i++) {
        check_one(names[i]);
        free(names[i]);
    }
    check_coverage();
    return VT_SUMMARY("test_golden");
}
