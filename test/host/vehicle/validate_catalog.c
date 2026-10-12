/*
 * validate_catalog — run the device validator and compiler over profile
 * files on the host.
 *
 *   validate_catalog [--signals contracts/signals.json] [--quiet] <dir|file>...
 *
 * A directory is treated as a catalog: every *.json in it is checked, the
 * file name must be "<id>.json" and ids must be unique. Each profile must
 * validate with zero errors, compile, pass vp_profile_check(), and every
 * formula is smoke-evaluated. Exit status 0 only if everything passes.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "vp_profile.h"
#include "vt_support.h"

#define MAX_FILES  128
#define MAX_ISSUES 64

static vp_validate_cfg_t s_cfg;
static int s_quiet;
static char s_ids[MAX_FILES][32];
static int s_n_ids;

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* returns 0 ok, 1 failed */
static int check_file(const char *path, int catalog_mode)
{
    size_t len = 0;
    char *text = vt_read_file(path, &len);
    vp_issue_t store[MAX_ISSUES];
    vp_issues_t l;
    vp_profile_t *p = NULL;
    vp_compile_err_t ce;
    int fail = 0;
    const char *base = strrchr(path, '/');

    base = base ? base + 1 : path;
    if (!text) {
        printf("FAIL %s: cannot read\n", path);
        return 1;
    }
    vp_issues_init(&l, store, MAX_ISSUES);
    vp_validate_json(text, len, &s_cfg, &l);
    if (l.n_errors) fail = 1;
    if (l.count && (fail || !s_quiet)) vt_print_issues(&l, "    ");

    if (!fail) {
        vp_issues_init(&l, store, MAX_ISSUES);
        ce = vp_compile_json(text, len, &s_cfg, &vt_malloc_allocator, &p, &l);
        if (ce != VP_COMPILE_OK || !p) {
            printf("    compile failed (%d)\n", (int)ce);
            vt_print_issues(&l, "    ");
            fail = 1;
        } else if (!vp_profile_check(p, p->total_size)) {
            printf("    compiled blob fails vp_profile_check\n");
            fail = 1;
        }
    }
    if (p) {
        /* smoke-evaluate every formula over a plausible buffer */
        uint8_t data[VP_MAX_DATA_LEN];
        uint16_t i;
        memset(data, 0x40, sizeof(data));
        for (i = 0; i < p->n_src; i++) {
            float v;
            vp_rpn_err_t e = vp_profile_eval_source(p, i, data, sizeof(data), &v);
            if (e != VP_RPN_OK && e != VP_RPN_ERR_RANGE && e != VP_RPN_ERR_LUT_MISS) {
                printf("    source %u: %s\n", (unsigned)i, vp_rpn_err_str(e));
                fail = 1;
            }
        }
        if (catalog_mode) {
            char want[48];
            int k;
            snprintf(want, sizeof(want), "%s.json", p->id);
            if (strcmp(base, want) != 0) {
                printf("    file name must be %s\n", want);
                fail = 1;
            }
            for (k = 0; k < s_n_ids; k++) {
                if (!strcmp(s_ids[k], p->id)) {
                    printf("    duplicate id %s\n", p->id);
                    fail = 1;
                }
            }
            if (s_n_ids < MAX_FILES) snprintf(s_ids[s_n_ids++], sizeof(s_ids[0]), "%s", p->id);
        }
        if (!s_quiet || fail) {
            printf("%s %s  id=%s rev=%u signals=%u requests=%u blob=%uB json=%uB\n", fail ? "FAIL" : "ok  ",
                   base, p->id, (unsigned)p->rev, (unsigned)p->n_sig, (unsigned)p->n_req,
                   (unsigned)p->total_size, (unsigned)len);
        }
        vp_profile_free(p, &vt_malloc_allocator);
    } else {
        printf("FAIL %s\n", base);
    }
    free(text);
    return fail;
}

static int check_dir(const char *dir, int *files, int *failed)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char *names[MAX_FILES];
    int n = 0, i;

    if (!d) {
        printf("FAIL %s: cannot open directory\n", dir);
        return -1;
    }
    while ((e = readdir(d)) != NULL && n < MAX_FILES) {
        if (e->d_name[0] != '.' && ends_with(e->d_name, ".json")) {
            size_t sz = strlen(dir) + strlen(e->d_name) + 2;
            names[n] = (char *)malloc(sz);
            snprintf(names[n], sz, "%s/%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);
    if (n == 0) {
        printf("FAIL %s: no *.json files\n", dir);
        return -1;
    }
    qsort(names, (size_t)n, sizeof(names[0]), cmp_str);
    for (i = 0; i < n; i++) {
        (*files)++;
        *failed += check_file(names[i], 1);
        free(names[i]);
    }
    return 0;
}

int main(int argc, char **argv)
{
    vt_registry_t reg = { NULL, 0, NULL };
    int i, files = 0, failed = 0, bad_args = 0;

    s_cfg.registry = vt_default_registry;
    s_cfg.n_registry = vt_default_registry_count;
    for (i = 1; i < argc; i++) {
        struct stat st;
        if (!strcmp(argv[i], "--signals") && i + 1 < argc) {
            if (vt_registry_load(argv[++i], &reg) != 0) return 2;
            s_cfg.registry = reg.defs;
            s_cfg.n_registry = reg.n;
            printf("signal registry: %s (%u obd.* keys)\n", argv[i], (unsigned)reg.n);
            continue;
        }
        if (!strcmp(argv[i], "--quiet")) {
            s_quiet = 1;
            continue;
        }
        if (stat(argv[i], &st) != 0) {
            printf("FAIL %s: not found\n", argv[i]);
            bad_args++;
        } else if (S_ISDIR(st.st_mode)) {
            if (check_dir(argv[i], &files, &failed) != 0) bad_args++;
        } else {
            files++;
            failed += check_file(argv[i], 0);
        }
    }
    if (files == 0 && bad_args == 0) {
        fprintf(stderr, "usage: %s [--signals signals.json] [--quiet] <dir|file>...\n", argv[0]);
        return 2;
    }
    printf("validate_catalog: %d files, %d ok, %d failed\n", files, files - failed, failed);
    vt_registry_free(&reg);
    return (failed || bad_args) ? 1 : 0;
}
