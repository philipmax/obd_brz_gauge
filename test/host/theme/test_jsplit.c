// theme_jsplit: streaming capture/split, chunk independence, long-string
// dropping, overflow, root arrays, early stop and malformed input.
#include <stdlib.h>
#include <string.h>

#include "theme_jsplit.h"
#include "th_util.h"

typedef struct {
    char log[4096];
    size_t len;
    int stop_after;   // stop when this many callbacks happened (0 = never)
    int calls;
} rec_t;

static bool rec_cb(void *ctx, tjs_kind_t kind, const char *key, uint32_t index,
                   const char *json, size_t len, bool overflow)
{
    rec_t *r = ctx;
    char line[1024];
    int n;
    if (overflow) {
        n = snprintf(line, sizeof(line), "%s %s[%u] OVERFLOW\n", kind == TJS_ITEM ? "I" : "V",
                     key, (unsigned)index);
    } else {
        TH_CHECK(json[len] == '\0' && strlen(json) == len);
        n = snprintf(line, sizeof(line), "%s %s[%u] %s\n", kind == TJS_ITEM ? "I" : "V", key,
                     (unsigned)index, json);
    }
    if (n > 0 && r->len + (size_t)n < sizeof(r->log)) {
        memcpy(r->log + r->len, line, (size_t)n);
        r->len += (size_t)n;
        r->log[r->len] = '\0';
    }
    r->calls++;
    return !(r->stop_after && r->calls >= r->stop_after);
}

static const char *const CAP[] = {"meta", "id", "s", NULL};
static const char *const SPLIT[] = {"pages", "assets", NULL};

// Run with a given chunk size; returns finish() and fills rec.
static bool run_chunked(const char *doc, size_t chunk, size_t max_str, size_t cap,
                        const char *const *cap_keys, const char *const *split_keys, rec_t *r)
{
    char *buf = malloc(cap ? cap : 1);
    memset(r->log, 0, sizeof(r->log));
    r->len = 0;
    r->calls = 0;
    tjs_cfg_t cfg = {cap_keys, split_keys, max_str, buf, cap, rec_cb, r};
    tjs_t s;
    tjs_init(&s, &cfg);
    size_t n = strlen(doc);
    for (size_t i = 0; i < n; i += chunk) {
        size_t k = n - i < chunk ? n - i : chunk;
        if (!tjs_feed(&s, doc + i, k)) break;
    }
    bool ok = tjs_finish(&s);
    free(buf);
    return ok;
}

static void test_basic(void)
{
    const char *doc =
        "\xEF\xBB\xBF{\n  \"schemaVersion\": \"1.0\",\n  \"meta\": {\"id\": \"my_theme\", "
        "\"name\": \"A \\\"q\\\" b\", \"n\": [1, 2, {\"x\": null}]},\n"
        "  \"skip\": [ {\"pages\": [1,2]}, \"}]\" ],\n"
        "  \"pages\": [ {\"id\": \"p1\", \"elements\": [ ]}, {\"id\" : \"p2\"} , 3, \"s\", true ],\n"
        "  \"assets\": [ {\"name\": \"bg\", \"dataUrl\": \"data:image/png;base64,AAAAAAAAAAAAAAAA\"} ],\n"
        "  \"id\": -1.5e3, \"s\": \"x\\u0041y\"\n}\n";
    const char *want =
        "V meta[0] {\"id\":\"my_theme\",\"name\":\"A \\\"q\\\" b\",\"n\":[1,2,{\"x\":null}]}\n"
        "I pages[0] {\"id\":\"p1\",\"elements\":[]}\n"
        "I pages[1] {\"id\":\"p2\"}\n"
        "I pages[2] 3\n"
        "I pages[3] \"s\"\n"
        "I pages[4] true\n"
        "I assets[0] {\"name\":\"bg\",\"dataUrl\":\"\"}\n"
        "V id[0] -1.5e3\n"
        "V s[0] \"x\\u0041y\"\n";
    const size_t chunks[] = {1, 2, 3, 7, 64, 100000};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        rec_t r = {0};
        bool ok = run_chunked(doc, chunks[i], 16, 1024, CAP, SPLIT, &r);
        TH_CHECK(ok);
        if (strcmp(r.log, want) != 0) {
            th_fail++;
            printf("FAIL chunk=%zu got:\n%s", chunks[i], r.log);
        }
    }
}

static void test_overflow_and_stop(void)
{
    const char *doc = "{\"pages\":[{\"id\":\"aaaaaaaaaaaa\"},{\"id\":\"b\"}],\"meta\":{\"k\":1}}";
    rec_t r = {0};
    TH_CHECK(run_chunked(doc, 5, 0, 12, CAP, SPLIT, &r));
    TH_CHECK(strcmp(r.log, "I pages[0] OVERFLOW\nI pages[1] {\"id\":\"b\"}\nV meta[0] {\"k\":1}\n") == 0);

    // stop after the first page: finish() still reports success
    rec_t r2 = {0};
    r2.stop_after = 1;
    TH_CHECK(run_chunked(doc, 3, 0, 256, CAP, SPLIT, &r2));
    TH_CHECK(r2.calls == 1);

    // split key with a non-array value falls back to capture (if listed) or is skipped
    static const char *const cap2[] = {"pages", NULL};
    rec_t r3 = {0};
    TH_CHECK(run_chunked("{\"pages\":{\"a\":[1]},\"assets\":7}", 4, 0, 256, cap2, SPLIT, &r3));
    TH_CHECK(strcmp(r3.log, "V pages[0] {\"a\":[1]}\n") == 0);

    // long keys never match
    static const char *const cap3[] = {"abcdefghijabcdefghijabcdefghijabcdefghij", "x", NULL};
    rec_t r4 = {0};
    TH_CHECK(run_chunked("{\"abcdefghijabcdefghijabcdefghijabcdefghij\":1,\"x\":2}", 1, 0, 64, cap3,
                         NULL, &r4));
    TH_CHECK(strcmp(r4.log, "V x[0] 2\n") == 0);

    // long string dropped inside nested values, escapes counted as bytes
    rec_t r5 = {0};
    TH_CHECK(run_chunked("{\"meta\":{\"a\":\"0123456789\",\"b\":\"01234\\\"\",\"c\":[\"0123456\",\"012345\"]}}", 2,
                         6, 256, CAP, NULL, &r5));
    TH_CHECK(strcmp(r5.log, "V meta[0] {\"a\":\"\",\"b\":\"\",\"c\":[\"\",\"012345\"]}\n") == 0);
}

static void test_root_array_and_errors(void)
{
    static const char *const root[] = {"", NULL};
    rec_t r = {0};
    TH_CHECK(run_chunked(" [ {\"a\":1} , [2,3], \"x\" ] ", 1, 0, 64, NULL, root, &r));
    TH_CHECK(strcmp(r.log, "I [0] {\"a\":1}\nI [1] [2,3]\nI [2] \"x\"\n") == 0);
    rec_t r1 = {0};
    TH_CHECK(run_chunked("[]", 1, 0, 64, NULL, root, &r1) && r1.calls == 0);

    // root array without "" in split_keys is an error
    rec_t r2 = {0};
    TH_CHECK(!run_chunked("[1]", 1, 0, 64, NULL, SPLIT, &r2));

    const char *bad[] = {
        "{\"a\":1", "{\"a\" 1}", "{\"a\":1]", "{\"a\":}", "{a:1}", "x", "{\"a\":1}}", "{\"a\":[1}",
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        rec_t rb = {0};
        bool ok = run_chunked(bad[i], 1, 0, 64, CAP, SPLIT, &rb);
        if (ok) {
            th_fail++;
            printf("FAIL malformed accepted: %s\n", bad[i]);
        }
    }

    // empty object, trailing whitespace
    rec_t r3 = {0};
    TH_CHECK(run_chunked("{ }\n\n", 1, 0, 64, CAP, SPLIT, &r3) && r3.calls == 0);

    // tjs_run convenience
    char buf[64];
    rec_t r4 = {0};
    tjs_cfg_t cfg = {CAP, SPLIT, 0, buf, sizeof(buf), rec_cb, &r4};
    TH_CHECK(tjs_run(&cfg, "{\"id\":\"q\"}", 10));
    TH_CHECK(strcmp(r4.log, "V id[0] \"q\"\n") == 0);
}

int main(void)
{
    test_basic();
    test_overflow_and_stop();
    test_root_array_and_errors();
    return th_report("jsplit");
}
