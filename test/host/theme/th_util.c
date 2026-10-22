#include "th_util.h"

#include <stdlib.h>

int th_fail = 0;
int th_checks = 0;

char *th_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

cJSON *th_load_json(const char *path)
{
    size_t len = 0;
    char *buf = th_read_file(path, &len);
    if (!buf) return NULL;
    cJSON *root = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!root) fprintf(stderr, "JSON parse error in %s\n", path);
    return root;
}

int th_report(const char *name)
{
    printf("%s: checks=%d fail=%d\n", name, th_checks, th_fail);
    return th_fail ? 1 : 0;
}
