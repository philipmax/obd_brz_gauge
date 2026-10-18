// theme_sidecar_core: gaugeproj → gauge-sidecar/1 reduction (streamed in
// chunks, multi-MB strings skipped), idempotent re-reduction, limits,
// stored-sidecar readers and pairing against a theme.bin element table.
#include <stdlib.h>
#include <string.h>

#include "theme_sidecar_core.h"
#include "th_util.h"

static int g_live;

static void *cnt_alloc(size_t n, void *ctx)
{
    (void)ctx;
    g_live++;
    return malloc(n);
}

static void cnt_free(void *p, void *ctx)
{
    (void)ctx;
    if (p) g_live--;
    free(p);
}

static const tsc_alloc_t A = {cnt_alloc, cnt_free, NULL};

static char *big_string(size_t n, char c)
{
    char *s = malloc(n + 1);
    memset(s, c, n);
    s[n] = '\0';
    return s;
}

static cJSON *elem(const char *id, const char *kind)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "id", id);
    cJSON_AddStringToObject(e, "kind", kind);
    cJSON_AddNumberToObject(e, "x", 10.5);
    cJSON_AddNumberToObject(e, "y", -3);
    return e;
}

static cJSON *rule(int i, int n_actions, int n_conds)
{
    char id[32];
    snprintf(id, sizeof(id), "r%d", i);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "id", id);
    cJSON_AddNumberToObject(r, "priority", i);
    cJSON *when = cJSON_AddObjectToObject(r, "when");
    cJSON *conds = cJSON_AddArrayToObject(when, "conditions");
    for (int c = 0; c < n_conds; c++) {
        cJSON *k = cJSON_CreateObject();
        cJSON_AddStringToObject(k, "source", "obd.rpm");
        cJSON_AddStringToObject(k, "op", ">");
        cJSON_AddNumberToObject(k, "value", 1000 + c);
        cJSON_AddItemToArray(conds, k);
    }
    cJSON *apply = cJSON_AddArrayToObject(r, "apply");
    for (int a = 0; a < n_actions; a++) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "target", "arc_1");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(x, "set"), "color", "#ff3158");
        cJSON_AddItemToArray(apply, x);
    }
    return r;
}

static cJSON *action(const char *target, const char *key, const char *val)
{
    cJSON *x = cJSON_CreateObject();
    cJSON_AddStringToObject(x, "target", target);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(x, "set"), key, val);
    return x;
}

// Editor-shaped project (JSON.stringify(project, null, 2) equivalent).
static char *make_project(const char *theme_id, int n_extra_pages, int extra_elems, size_t big)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "schemaVersion", "1.0");
    cJSON *meta = cJSON_AddObjectToObject(p, "meta");
    cJSON_AddStringToObject(meta, "id", theme_id);
    cJSON_AddStringToObject(meta, "name", "My \"Theme\"\n");
    cJSON_AddStringToObject(meta, "version", "1.2.0");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "colors"), "bg", "#000000");

    cJSON *pages = cJSON_AddArrayToObject(p, "pages");
    cJSON *pg = cJSON_CreateObject();
    cJSON_AddStringToObject(pg, "id", "main_gauge");
    cJSON_AddStringToObject(pg, "backgroundColor", "#101010");
    cJSON *els = cJSON_AddArrayToObject(pg, "elements");

    cJSON *e = elem("arc_1", "arc");
    cJSON *dd = cJSON_AddObjectToObject(e, "dataDisplay");
    cJSON_AddNumberToObject(dd, "displayMin", 0);
    cJSON_AddNumberToObject(dd, "displayMax", 7000);
    cJSON_AddNumberToObject(dd, "foo", 1);
    cJSON_AddNumberToObject(e, "rotation", 45);   // arc rotation is baked into theme.bin
    cJSON_AddItemToArray(els, e);

    e = elem("lbl_1", "label");
    cJSON_AddNumberToObject(e, "rotation", 0);
    dd = cJSON_AddObjectToObject(e, "dataDisplay");
    cJSON_AddStringToObject(dd, "unit", "RPM");
    cJSON_AddNumberToObject(dd, "precision", 1);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(dd, "valueMap"), "0", "N");
    cJSON_AddItemToArray(els, e);

    e = elem("lbl_2", "label");
    cJSON_AddNumberToObject(e, "rotation", 15);
    cJSON_AddItemToArray(els, e);

    e = elem("bar_1", "bar");
    cJSON_AddNumberToObject(e, "rotation", 0);
    cJSON_AddStringToObject(e, "direction", "vertical");
    cJSON_AddItemToArray(els, e);

    e = elem("img_1", "image");
    cJSON_AddNumberToObject(e, "rotation", 30);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(e, "dataDisplay"), "unit", "x");
    cJSON_AddItemToArray(els, e);

    e = elem("ptr_1", "pointer");
    cJSON_AddObjectToObject(e, "dataDisplay");
    cJSON_AddItemToArray(els, e);

    cJSON_AddItemToArray(els, elem("vid_intro", "image"));

    cJSON *rules = cJSON_AddArrayToObject(pg, "rules");
    cJSON *r0 = rule(0, 0, 6);
    cJSON *ap = cJSON_GetObjectItem(r0, "apply");
    cJSON_AddItemToArray(ap, action("arc_1", "color", "#00ff00"));
    cJSON_AddItemToArray(ap, action("missing", "text", "x"));
    cJSON_AddItemToArray(ap, action("img_1", "asset", "needle2"));
    cJSON_AddItemToArray(ap, action("lbl_2", "visible", "x"));
    cJSON_AddItemToArray(ap, action("img_1", "asset", "nope"));
    cJSON_AddItemToArray(rules, r0);
    cJSON_AddItemToArray(rules, rule(1, 10, 1));
    for (int i = 2; i < 18; i++) cJSON_AddItemToArray(rules, rule(i, 1, 1));
    cJSON_AddItemToArray(pages, pg);

    pg = cJSON_CreateObject();
    cJSON_AddStringToObject(pg, "id", "page2");
    cJSON_AddArrayToObject(pg, "elements");
    cJSON_AddItemToArray(pages, pg);

    pg = cJSON_CreateObject();   // duplicate id: dropped
    cJSON_AddStringToObject(pg, "id", "main_gauge");
    cJSON_AddItemToArray(pages, pg);
    pg = cJSON_CreateObject();   // no id: dropped
    cJSON_AddArrayToObject(pg, "elements");
    cJSON_AddItemToArray(pages, pg);

    for (int i = 0; i < n_extra_pages; i++) {
        char id[32];
        snprintf(id, sizeof(id), "extra_%d", i);
        pg = cJSON_CreateObject();
        cJSON_AddStringToObject(pg, "id", id);
        els = cJSON_AddArrayToObject(pg, "elements");
        for (int k = 0; k < extra_elems; k++) {
            char eid[32];
            snprintf(eid, sizeof(eid), "label_%d_%d", i, k);
            e = elem(eid, "label");
            cJSON_AddStringToObject(cJSON_AddObjectToObject(e, "dataDisplay"), "unit", "km/h");
            cJSON_AddItemToArray(els, e);
        }
        cJSON_AddItemToArray(pages, pg);
    }

    cJSON *assets = cJSON_AddArrayToObject(p, "assets");
    const char *names[][2] = {{"bg", "bg.png"}, {"needle2", "needle2.png"}, {"intro", "intro.mjpeg"}};
    for (int i = 0; i < 3; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", names[i][0]);
        cJSON_AddStringToObject(a, "fileName", names[i][1]);
        cJSON_AddNumberToObject(a, "width", 360);
        cJSON_AddStringToObject(a, "format", "rgba8888");
        char *s = big_string(i == 0 ? big : 100, 'A');
        cJSON_AddStringToObject(a, "dataUrl", s);
        free(s);
        cJSON_AddItemToArray(assets, a);
    }
    cJSON *fonts = cJSON_AddArrayToObject(p, "fonts");
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "name", "dseg");
    char *s = big_string(big / 2, 'B');
    cJSON_AddStringToObject(cJSON_AddObjectToObject(f, "compiled"), "26", s);
    free(s);
    cJSON_AddItemToArray(fonts, f);

    char *txt = cJSON_Print(p);
    cJSON_Delete(p);
    return txt;
}

static tsc_err_t reduce(const char *doc, size_t chunk, char **out, size_t *out_len, tsc_stats_t *st)
{
    tsc_builder_t *b = tsc_builder_new(&A);
    TH_CHECK(b != NULL);
    size_t n = strlen(doc);
    for (size_t i = 0; i < n; i += chunk) {
        size_t k = n - i < chunk ? n - i : chunk;
        if (!tsc_builder_feed(b, doc + i, k)) break;
    }
    tsc_err_t err = tsc_builder_finish(b, n, "abc123", out, out_len, st);
    tsc_builder_free(b);
    return err;
}

static bool asset_exists(const char *name, void *ctx)
{
    (void)ctx;
    return strcmp(name, "needle2") == 0 || strcmp(name, "bg") == 0;
}

static void test_reduce_and_read(void)
{
    char *proj = make_project("my_theme", 0, 0, 300000);
    char *out = NULL;
    size_t out_len = 0;
    tsc_stats_t st;
    TH_CHECK(reduce(proj, 4096, &out, &out_len, &st) == TSC_OK);
    TH_CHECK(out && strlen(out) == out_len && out_len <= TSC_MAX_BYTES);
    TH_CHECK(!st.reduced_input && strcmp(st.theme_id, "my_theme") == 0 &&
             strcmp(st.version, "1.2.0") == 0);
    TH_CHECK(st.pages == 2 && st.pages_invalid == 2 && st.pages_too_big == 0);
    TH_CHECK(st.rules == 16 && st.rules_truncated == 2);
    TH_CHECK(st.elements == 7 && st.overlays == 4 && st.asset_files == 3);

    // same result with 1-byte and 7-byte chunks
    char *o1 = NULL, *o7 = NULL;
    size_t l1 = 0, l7 = 0;
    TH_CHECK(reduce(proj, 1, &o1, &l1, NULL) == TSC_OK && o1 && strcmp(o1, out) == 0);
    TH_CHECK(reduce(proj, 7, &o7, &l7, NULL) == TSC_OK && o7 && strcmp(o7, out) == 0);
    cnt_free(o1, NULL);
    cnt_free(o7, NULL);

    cJSON *sc = cJSON_Parse(out);
    TH_CHECK(sc != NULL);
    TH_CHECK(strcmp(cJSON_GetObjectItem(sc, "format")->valuestring, "gauge-sidecar/1") == 0);
    TH_CHECK(strcmp(cJSON_GetObjectItem(sc, "theme_id")->valuestring, "my_theme") == 0);
    const cJSON *src = cJSON_GetObjectItem(sc, "source");
    TH_CHECK(strcmp(cJSON_GetObjectItem(src, "kind")->valuestring, "gaugeproj") == 0);
    TH_CHECK(cJSON_GetObjectItem(src, "size")->valuedouble == (double)strlen(proj));
    TH_CHECK(strcmp(cJSON_GetObjectItem(src, "sha256")->valuestring, "abc123") == 0);
    TH_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(sc, "asset_files"), "intro")->valuestring,
                    "intro.mjpeg") == 0);
    const cJSON *pages = cJSON_GetObjectItem(sc, "pages");
    TH_CHECK(cJSON_GetArraySize(pages) == 2);
    const cJSON *p0 = cJSON_GetArrayItem(pages, 0);
    TH_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(p0, "element_ids")) == 7);
    TH_CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(p0, "elements"), "ptr_1")->valuestring,
                    "pointer") == 0);
    const cJSON *ov = cJSON_GetObjectItem(p0, "overlay");
    TH_CHECK(cJSON_GetArraySize(ov) == 4);
    const cJSON *arc = cJSON_GetObjectItem(ov, "arc_1");
    TH_CHECK(arc && !cJSON_GetObjectItem(arc, "rotation"));
    TH_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(arc, "dataDisplay"), "displayMax")->valuedouble == 7000);
    TH_CHECK(!cJSON_GetObjectItem(cJSON_GetObjectItem(arc, "dataDisplay"), "foo"));
    TH_CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(ov, "lbl_2"), "rotation")->valuedouble == 15);
    TH_CHECK(!cJSON_GetObjectItem(cJSON_GetObjectItem(ov, "lbl_1"), "rotation"));
    const cJSON *bar = cJSON_GetObjectItem(ov, "bar_1");
    TH_CHECK(bar && !cJSON_GetObjectItem(bar, "rotation") &&
             strcmp(cJSON_GetObjectItem(bar, "direction")->valuestring, "vertical") == 0);
    TH_CHECK(!cJSON_GetObjectItem(ov, "img_1") && !cJSON_GetObjectItem(ov, "ptr_1"));
    TH_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(p0, "rules")) == 16);

    // rules are verbatim
    cJSON *orig = cJSON_Parse(proj);
    const cJSON *orules = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(orig, "pages"), 0),
                                              "rules");
    const cJSON *srules = cJSON_GetObjectItem(p0, "rules");
    bool same = true;
    for (int i = 0; i < 16; i++) {
        same = same && cJSON_Compare(cJSON_GetArrayItem(orules, i), cJSON_GetArrayItem(srules, i), true);
    }
    TH_CHECK(same);
    cJSON_Delete(orig);
    cJSON_Delete(sc);

    // a reduced document reduces to itself (source passes through)
    char *again = NULL;
    size_t again_len = 0;
    tsc_stats_t st2;
    TH_CHECK(reduce(out, 333, &again, &again_len, &st2) == TSC_OK);
    TH_CHECK(again && strcmp(again, out) == 0 && st2.reduced_input && st2.pages == 2);
    cnt_free(again, NULL);

    // readers
    char tid[72], ver[32];
    uint16_t np = 0;
    TH_CHECK(tsc_read_header(out, out_len, tid, sizeof(tid), ver, sizeof(ver), &np));
    TH_CHECK(strcmp(tid, "my_theme") == 0 && strcmp(ver, "1.2.0") == 0 && np == 2);
    TH_CHECK(!tsc_read_header(proj, strlen(proj), tid, sizeof(tid), ver, sizeof(ver), &np));

    char *scratch = malloc(2 * TSC_MAX_BYTES + 64);
    tsc_page_t pg;
    TH_CHECK(tsc_find_page(out, out_len, "page2", 0, scratch, 2 * TSC_MAX_BYTES + 64, &pg));
    TH_CHECK(strcmp(pg.id, "page2") == 0 && pg.rules_len == 2 && pg.elements_len == 2);
    TH_CHECK(!tsc_find_page(out, out_len, "nope", 0, scratch, 2 * TSC_MAX_BYTES + 64, &pg));
    TH_CHECK(tsc_find_page(out, out_len, NULL, 0, scratch, 2 * TSC_MAX_BYTES + 64, &pg));
    TH_CHECK(strcmp(pg.id, "main_gauge") == 0 && pg.overlay && pg.overlay[0] == '{');
    TH_CHECK(pg.element_ids[pg.element_ids_len] == '\0' && pg.rules[pg.rules_len] == '\0');

    char file[64];
    TH_CHECK(tsc_asset_file(out, out_len, "needle2", scratch, TSC_ITEM_MAX, file, sizeof(file)));
    TH_CHECK(strcmp(file, "needle2.png") == 0);
    TH_CHECK(!tsc_asset_file(out, out_len, "zzz", scratch, TSC_ITEM_MAX, file, sizeof(file)));

    // pairing (pg = main_gauge)
    tr_element_t bin[] = {
        {"arc_1", TR_KIND_ARC}, {"lbl_1", TR_KIND_LABEL}, {"lbl_2", TR_KIND_ARC},
        {"bar_1", TR_KIND_BAR}, {"img_1", TR_KIND_IMAGE}, {"ptr_1", TR_KIND_POINTER},
        {"vid_intro", TR_KIND_IMAGE},
    };
    tsc_pair_t pr;
    tsc_pair_page(&pg, bin, 7, asset_exists, NULL, &pr);
    TH_CHECK(pr.sc_elements == 7 && pr.matched == 6 && pr.kind_mismatch == 1 && pr.missing == 0);
    TH_CHECK(pr.in_sync);
    TH_CHECK(pr.rules == 16 && pr.actions == 5 + 8 + 14 && pr.actions_truncated == 2);
    TH_CHECK(pr.actions_no_target == 2 && pr.actions_no_asset == 1 && pr.conds_truncated == 2);
    TH_CHECK(pr.overlays == 4 && pr.overlays_unmatched == 1);
    // reordered / shortened bin → out of sync, missing ids
    tsc_pair_page(&pg, bin + 1, 6, NULL, NULL, &pr);
    TH_CHECK(!pr.in_sync && pr.missing == 1 && pr.actions_no_target == 2 + 14 + 8 + 1);
    free(scratch);

    cnt_free(out, NULL);
    free(proj);
    TH_CHECK(g_live == 0);
}

static void test_limits_and_errors(void)
{
    // 40 extra pages: over TSC_MAX_PAGES, and the kept ones exceed 64 KB
    char *proj = make_project("Big-Theme_2", 40, 40, 1000);
    char *out = NULL;
    size_t out_len = 0;
    tsc_stats_t st;
    TH_CHECK(reduce(proj, 1024, &out, &out_len, &st) == TSC_OK);
    TH_CHECK(out && out_len <= TSC_MAX_BYTES);
    // 42 valid pages: kept + too big (64 KB) + beyond the 32-page limit
    TH_CHECK(st.pages_invalid == 2 && st.pages_too_big > 0);
    TH_CHECK(st.pages + st.pages_too_big + st.pages_over_limit == 42);
    TH_CHECK(st.pages <= TSC_MAX_PAGES);
    cJSON *sc = cJSON_Parse(out);
    TH_CHECK(sc != NULL);
    TH_CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(sc, "pages")) == st.pages);
    cJSON_Delete(sc);
    cnt_free(out, NULL);
    free(proj);

    // small pages: only the 32-page limit applies
    proj = make_project("Many", 40, 1, 10);
    TH_CHECK(reduce(proj, 1024, &out, &out_len, &st) == TSC_OK);
    TH_CHECK(st.pages == TSC_MAX_PAGES && st.pages_over_limit == 10 && st.pages_too_big == 0);
    cnt_free(out, NULL);
    free(proj);

    proj = make_project("1bad", 0, 0, 10);
    TH_CHECK(reduce(proj, 512, &out, &out_len, &st) == TSC_ERR_THEME_ID && out == NULL);
    free(proj);
    proj = make_project("a2345678901234567890123456789012345678901234567890123456789012345", 0, 0, 10);
    TH_CHECK(reduce(proj, 512, &out, &out_len, &st) == TSC_ERR_THEME_ID);
    free(proj);

    TH_CHECK(reduce("{\"format\":\"gauge-sidecar/2\",\"theme_id\":\"a\",\"pages\":[]}", 9, &out,
                    &out_len, &st) == TSC_ERR_FORMAT);
    TH_CHECK(reduce("{\"meta\":{\"id\":\"a\"},\"pages\":[", 9, &out, &out_len, &st) == TSC_ERR_JSON);
    TH_CHECK(reduce("[1,2]", 9, &out, &out_len, &st) == TSC_ERR_JSON);
    TH_CHECK(reduce("{\"meta\":{\"id\":\"ok\"}}", 9, &out, &out_len, &st) == TSC_OK);
    TH_CHECK(out && strcmp(out, "{\"format\":\"gauge-sidecar/1\",\"theme_id\":\"ok\",\"project_version\":"
                                "\"\",\"source\":{\"kind\":\"gaugeproj\",\"size\":20,\"sha256\":"
                                "\"abc123\"},\"asset_files\":{},\"pages\":[]}") == 0);
    cnt_free(out, NULL);

    TH_CHECK(tsc_valid_theme_id("a") && tsc_valid_theme_id("my_theme-2"));
    TH_CHECK(!tsc_valid_theme_id("") && !tsc_valid_theme_id("_a") && !tsc_valid_theme_id("a/b") &&
             !tsc_valid_theme_id("a.b") && !tsc_valid_theme_id(NULL));
    TH_CHECK(g_live == 0);
}

int main(void)
{
    test_reduce_and_read();
    test_limits_and_errors();
    return th_report("sidecar");
}
