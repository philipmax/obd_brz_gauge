#include "vt_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

const vp_sig_def_t vt_default_registry[] = {
    { "obd.rpm",             0x0001 },
    { "obd.speed",           0x0002 },
    { "obd.coolant_temp",    0x0003 },
    { "obd.oil_temp",        0x0004 },
    { "obd.intake_temp",     0x0005 },
    { "obd.throttle",        0x0006 },
    { "obd.load",            0x0007 },
    { "obd.boost",           0x0008 },
    { "obd.oil_pressure",    0x0009 },
    { "obd.battery_voltage", 0x000A },
    { "obd.afr",             0x000B },
    { "obd.gear",            0x000C },
    { "obd.brake_temp",      0x000D },
    { "obd.fuel_level",      0x000E },
};
const size_t vt_default_registry_count = sizeof(vt_default_registry) / sizeof(vt_default_registry[0]);

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

static const char *entry_key(const cJSON *e)
{
    const char *k;
    if (cJSON_IsString(e)) return e->valuestring;
    k = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "key"));
    if (k) return k;
    k = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "id"));
    if (k && !strncmp(k, "obd.", 4)) return k;
    return NULL;
}

static unsigned entry_num(const cJSON *e)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "num_id");
    if (!v) v = cJSON_GetObjectItemCaseSensitive(e, "id");
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 0xFFFF) return (unsigned)v->valuedouble;
    if (cJSON_IsString(v) && (!strncmp(v->valuestring, "0x", 2) || !strncmp(v->valuestring, "0X", 2))) {
        return (unsigned)strtoul(v->valuestring, NULL, 16) & 0xFFFFu;
    }
    return 0;
}

int vt_registry_load(const char *path, vt_registry_t *reg)
{
    size_t len = 0, n = 0, bytes = 0, pos = 0;
    char *text = vt_read_file(path, &len);
    cJSON *root, *arr, *e;

    memset(reg, 0, sizeof(*reg));
    if (!text) {
        fprintf(stderr, "cannot read %s\n", path);
        return -1;
    }
    root = cJSON_ParseWithLength(text, len);
    free(text);
    if (!root) {
        fprintf(stderr, "%s: not valid JSON\n", path);
        return -1;
    }
    arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "signals");
    cJSON_ArrayForEach(e, arr) {
        const char *k = entry_key(e);
        if (k && !strncmp(k, "obd.", 4)) {
            n++;
            bytes += strlen(k) + 1;
        }
    }
    if (n == 0) {
        fprintf(stderr, "%s: no obd.* signal keys found\n", path);
        cJSON_Delete(root);
        return -1;
    }
    reg->defs = (vp_sig_def_t *)calloc(n, sizeof(vp_sig_def_t));
    reg->strings = (char *)malloc(bytes);
    if (!reg->defs || !reg->strings) {
        vt_registry_free(reg);
        cJSON_Delete(root);
        return -1;
    }
    cJSON_ArrayForEach(e, arr) {
        const char *k = entry_key(e);
        if (k && !strncmp(k, "obd.", 4)) {
            size_t kl = strlen(k) + 1;
            memcpy(reg->strings + pos, k, kl);
            reg->defs[reg->n].key = reg->strings + pos;
            reg->defs[reg->n].num_id = (uint16_t)entry_num(e);
            reg->n++;
            pos += kl;
        }
    }
    cJSON_Delete(root);
    return 0;
}

void vt_registry_free(vt_registry_t *reg)
{
    free(reg->defs);
    free(reg->strings);
    memset(reg, 0, sizeof(*reg));
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
