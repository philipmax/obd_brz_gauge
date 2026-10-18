#include "vt_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

char *vt_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    if (len) *len = (size_t)n;
    return buf;
}

static void *vt_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void vt_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

const vp_allocator_t vt_malloc_allocator = { vt_alloc, vt_free, NULL };

void vt_print_issues(const vp_issues_t *l, const char *prefix)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        const vp_issue_t *it = &l->items[i];
        printf("%s%s %s: %s\n", prefix ? prefix : "", it->code, it->path, it->msg);
    }
    if (l->n_errors + l->n_warnings > l->count) {
        printf("%s... %u more\n", prefix ? prefix : "",
               (unsigned)(l->n_errors + l->n_warnings - l->count));
    }
}
