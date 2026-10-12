#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const tu_sources[TU_N_SOURCES] = {
    "obd.rpm", "obd.speed", "obd.boost", "obd.coolant_temp", "obd.oil_pressure",
    "obd.gear", "obd.battery_voltage", "obd.oil_temp", "obd.afr", "obd.throttle",
    "obd.intake_temp",
};

int tu_resolve(const char *key, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < TU_N_SOURCES; i++) {
        if (strcmp(key, tu_sources[i]) == 0) return i;
    }
    return -1;
}

const char *tu_first_existing(const char *const *paths)
{
    for (; *paths; paths++) {
        FILE *f = fopen(*paths, "rb");
        if (f) {
            fclose(f);
            return *paths;
        }
    }
    return NULL;
}

cJSON *tu_load_json(const char *path)
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
    cJSON *root = cJSON_ParseWithLength(buf, got);
    free(buf);
    if (!root) fprintf(stderr, "JSON parse error in %s\n", path);
    return root;
}
