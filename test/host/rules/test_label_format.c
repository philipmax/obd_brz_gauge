// Label formatter vectors: cases [{ "format": str|null, "divisor": num|null, "raw": num, "text": str }],
// either as a bare array (fixtures/label_vectors.json, tools/gen_fixtures.mjs) or under "cases"
// (contracts/fixtures/format/label_format.json). Both are generated from the editor's themePacker.ts.
//
//   test_label_format [--expect-cases N] <file.json> [fallback...]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tr_format.h"
#include "test_util.h"

int main(int argc, char **argv)
{
    const char *paths[16];
    int n = 0, expect_cases = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--expect-cases") == 0 && i + 1 < argc) expect_cases = atoi(argv[++i]);
        else if (n < 15) paths[n++] = argv[i];
    }
    paths[n] = NULL;
    const char *path = tu_first_existing(paths);
    if (!path) {
        fprintf(stderr, "no label vectors file found\n");
        return 2;
    }
    cJSON *root = tu_load_json(path);
    const cJSON *cases = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "cases") : root;
    if (!cJSON_IsArray(cases)) {
        cJSON_Delete(root);
        return 2;
    }
    printf("label vectors: %s\n", path);

    int pass = 0, fail = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cases) {
        const cJSON *jf = cJSON_GetObjectItemCaseSensitive(c, "format");
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(c, "divisor");
        const cJSON *jr = cJSON_GetObjectItemCaseSensitive(c, "raw");
        const cJSON *jt = cJSON_GetObjectItemCaseSensitive(c, "text");
        const char *fmt = cJSON_IsString(jf) ? jf->valuestring : NULL;
        double divisor = cJSON_IsNumber(jd) ? jd->valuedouble : 0.0;
        double raw = cJSON_IsNumber(jr) ? jr->valuedouble : 0.0;
        const char *want = cJSON_IsString(jt) ? jt->valuestring : "";
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(c, "name");

        char out[256];
        size_t len = tr_fmt_label(out, sizeof(out), fmt, divisor, raw);
        if (strcmp(out, want) == 0 && len == strlen(want)) {
            pass++;
        } else {
            fail++;
            printf("FAIL %s format=%s divisor=%.17g raw=%.17g: got \"%s\" want \"%s\"\n",
                   cJSON_IsString(jn) ? jn->valuestring : "-", fmt ? fmt : "(null)", divisor, raw, out, want);
        }
    }
    cJSON_Delete(root);
    printf("label cases=%d pass=%d fail=%d\n", pass + fail, pass, fail);
    if (expect_cases >= 0 && pass + fail != expect_cases) {
        printf("FAIL expected %d cases, file has %d\n", expect_cases, pass + fail);
        return 1;
    }
    return fail ? 1 : 0;
}
