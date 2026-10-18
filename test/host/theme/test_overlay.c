// Overlay number path: toFixed, Number::toString, transformDataValue, range,
// clamp01, pointer angle and label text against fixtures/overlay_vectors.json
// (tools/gen_overlay_vectors.py, ECMA-262 via Python Decimal).
//
//   test_overlay <overlay_vectors.json>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "theme_overlay.h"
#include "th_util.h"

static double jnum(const cJSON *v)
{
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v)) {
        if (strcmp(v->valuestring, "NaN") == 0) return NAN;
        if (strcmp(v->valuestring, "Infinity") == 0) return INFINITY;
        if (strcmp(v->valuestring, "-Infinity") == 0) return -INFINITY;
    }
    return NAN;
}

static bool same_double(double a, double b)
{
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    return a == b && signbit(a) == signbit(b);
}

// {"dd": {...}|null} → overlay entry {"kind":"label","dataDisplay":dd}
static theme_overlay_t *mk_overlay(const cJSON *dd)
{
    if (!cJSON_IsObject(dd)) return NULL;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "kind", "label");
    cJSON_AddItemToObject(e, "dataDisplay", cJSON_Duplicate(dd, true));
    theme_overlay_t *ov = NULL;
    TH_CHECK(theme_overlay_parse(e, NULL, &ov));
    cJSON_Delete(e);
    return ov;
}

static void test_vectors(const char *path)
{
    cJSON *root = th_load_json(path);
    TH_CHECK(root != NULL);
    if (!root) return;

    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "to_fixed")) {
        double x = jnum(cJSON_GetArrayItem(c, 0));
        int f = (int)cJSON_GetArrayItem(c, 1)->valuedouble;
        const char *want = cJSON_GetArrayItem(c, 2)->valuestring;
        char out[256];
        size_t len = theme_js_to_fixed(x, f, out, sizeof(out));
        if (strcmp(out, want) != 0 || len != strlen(want)) {
            th_fail++;
            printf("FAIL toFixed(%.17g, %d): got \"%s\" want \"%s\"\n", x, f, out, want);
        }
        n++;
    }
    printf("to_fixed cases=%d\n", n);
    TH_CHECK(n > 400);

    n = 0;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "num_to_str")) {
        double x = jnum(cJSON_GetArrayItem(c, 0));
        const char *want = cJSON_GetArrayItem(c, 1)->valuestring;
        char out[64];
        theme_js_num_to_str(x, out, sizeof(out));
        if (strcmp(out, want) != 0) {
            th_fail++;
            printf("FAIL String(%.17g): got \"%s\" want \"%s\"\n", x, out, want);
        }
        n++;
    }
    printf("num_to_str cases=%d\n", n);
    TH_CHECK(n > 300);

    n = 0;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "transform")) {
        theme_overlay_t *ov = mk_overlay(cJSON_GetObjectItem(c, "dd"));
        double raw = jnum(cJSON_GetObjectItem(c, "raw"));
        const cJSON *jr = cJSON_GetObjectItem(c, "range");
        double v = theme_ov_transform(ov, raw);
        double lo, hi;
        // the generator uses range_min 0, range_max 8000 unless degenerate (dd null)
        if (ov) theme_ov_range(ov, 0.0, 8000.0, &lo, &hi);
        else { lo = jnum(cJSON_GetArrayItem(jr, 0)); hi = jnum(cJSON_GetArrayItem(jr, 1)); }
        if (!ov && !cJSON_IsNull(cJSON_GetObjectItem(c, "dd"))) th_fail++;
        double p = theme_ov_progress(v, lo, hi);
        double a = theme_ov_pointer_angle(v, lo, hi, -135.0, 135.0);
        bool ok = same_double(v, jnum(cJSON_GetObjectItem(c, "value"))) &&
                  same_double(lo, jnum(cJSON_GetArrayItem(jr, 0))) &&
                  same_double(hi, jnum(cJSON_GetArrayItem(jr, 1))) &&
                  same_double(p, jnum(cJSON_GetObjectItem(c, "progress"))) &&
                  same_double(a, jnum(cJSON_GetObjectItem(c, "angle")));
        if (!ok) {
            th_fail++;
            char *s = cJSON_PrintUnformatted(c);
            printf("FAIL transform %s: v=%.17g lo=%.17g hi=%.17g p=%.17g a=%.17g\n", s, v, lo, hi, p, a);
            free(s);
        }
        theme_overlay_free(ov, NULL);
        n++;
    }
    printf("transform cases=%d\n", n);
    TH_CHECK(n > 100);

    n = 0;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(root, "label")) {
        theme_overlay_t *ov = mk_overlay(cJSON_GetObjectItem(c, "dd"));
        const cJSON *fmt = cJSON_GetObjectItem(c, "format");
        double divisor = jnum(cJSON_GetObjectItem(c, "divisor"));
        double raw = jnum(cJSON_GetObjectItem(c, "raw"));
        const char *want = cJSON_GetObjectItem(c, "text")->valuestring;
        char out[160];
        theme_ov_label_text(ov, cJSON_IsString(fmt) ? fmt->valuestring : NULL, divisor, raw, out,
                            sizeof(out));
        if (strcmp(out, want) != 0) {
            th_fail++;
            printf("FAIL label raw=%.17g: got \"%s\" want \"%s\"\n", raw, out, want);
        }
        theme_overlay_free(ov, NULL);
        n++;
    }
    printf("label cases=%d\n", n);
    TH_CHECK(n > 20);
    cJSON_Delete(root);
}

static void test_parse_edges(void)
{
    // null fields: `??` falls back, `!== undefined` is present (null - null = 0)
    cJSON *e = cJSON_Parse("{\"kind\":\"bar\",\"rotation\":90,\"direction\":\"vertical\","
                           "\"dataDisplay\":{\"scale\":null,\"offset\":2,\"inputMin\":null,"
                           "\"inputMax\":null,\"displayMin\":0,\"displayMax\":10,"
                           "\"precision\":2.7,\"unit\":\"\",\"valueMap\":{\"3\":\"a\",\"3\":\"b\"}}}");
    theme_overlay_t *ov = NULL;
    TH_CHECK(theme_overlay_parse(e, NULL, &ov));
    TH_CHECK(ov && ov->kind == 2 /* TR_KIND_BAR */ && ov->vertical && ov->has_rotation &&
             ov->rotation == 90);
    // inputMin === inputMax (null === null) → no mapping; scale null → 1
    TH_CHECK(theme_ov_transform(ov, 1.0) == 3.0);
    TH_CHECK(ov->precision == 2 && ov->unit == NULL);
    char out[64];
    theme_ov_label_text(ov, NULL, 0, 1.0, out, sizeof(out));
    TH_CHECK(strcmp(out, "b") == 0);   // last duplicate wins
    theme_ov_label_text(ov, NULL, 0, 0.5, out, sizeof(out));
    TH_CHECK(strcmp(out, "2.50") == 0);
    theme_ov_label_text(ov, NULL, 0, NAN, out, sizeof(out));
    TH_CHECK(strcmp(out, "--") == 0);
    theme_overlay_free(ov, NULL);
    cJSON_Delete(e);

    // inputMin null vs inputMax 0 differ → mapped with null read as 0
    e = cJSON_Parse("{\"kind\":\"arc\",\"dataDisplay\":{\"inputMin\":null,\"inputMax\":10,"
                    "\"displayMin\":0,\"displayMax\":100}}");
    TH_CHECK(theme_overlay_parse(e, NULL, &ov));
    TH_CHECK(theme_ov_transform(ov, 5.0) == 50.0);
    theme_overlay_free(ov, NULL);
    cJSON_Delete(e);

    // no overlay: identity, label via the formatter
    theme_ov_label_text(NULL, "%d.%01d", 10, 123, out, sizeof(out));
    TH_CHECK(strcmp(out, "12.3") == 0);
    theme_ov_label_text(NULL, NULL, 0, -7.9, out, sizeof(out));
    TH_CHECK(strcmp(out, "-7") == 0);

    TH_CHECK(isnan(theme_ov_progress(5, 5, 5)));
    TH_CHECK(theme_ov_progress(6, 5, 5) == 1.0);
    TH_CHECK(theme_ov_progress(4, 5, 5) == 0.0);
    TH_CHECK(isnan(theme_ov_progress(NAN, 0, 1)));
    TH_CHECK(isnan(theme_ov_pointer_angle(NAN, 0, 1, 0, 90)));
    TH_CHECK(theme_ov_pointer_angle(3, 5, 5, -10, 90) == -10);

    // tiny buffers never overflow
    char small[4];
    theme_js_to_fixed(123456.789, 3, small, sizeof(small));
    TH_CHECK(strcmp(small, "123") == 0);
    theme_js_num_to_str(-1e-7, small, sizeof(small));
    TH_CHECK(strcmp(small, "-1e") == 0);
    theme_js_to_fixed(0.5, 100, out, sizeof(out));
    TH_CHECK(strlen(out) == sizeof(out) - 1);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_overlay <overlay_vectors.json>\n");
        return 2;
    }
    test_vectors(argv[1]);
    test_parse_edges();
    return th_report("overlay");
}
