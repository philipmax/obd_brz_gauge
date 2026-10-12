/*
 * dump_legacy — extract the legacy vehicle_profiles table to intermediate JSON.
 *
 *   cc -I../../main -I../../main/app_obd_dsp dump_legacy.c -o dump_legacy -lm
 *   ./dump_legacy > legacy_profiles.json
 *
 * Each object in the output array covers one legacy profile plus any
 * matching override from vehicle_custom_config.h.  The Python converter
 * legacy_to_v1.py reads this file and produces skygauge.vehicle/1 JSON.
 *
 * NOTE: this file deliberately #includes the firmware C source to share the
 * exact same table at compile time.  It does not link any FreeRTOS or
 * ESP-IDF symbols — the shimmed headers below satisfy every dependency.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  Shims for symbols the firmware headers reference but we don't need  */
/* ------------------------------------------------------------------ */

/* obd_data_cache.h needs enGear */
typedef enum { GEAR_N=0,GEAR_1,GEAR_2,GEAR_3,GEAR_4,GEAR_5,GEAR_6,GEAR_7,GEAR_8,GEAR_R } enGear;

/* Dummy obd_data_cache.h inclusion guard — provide the minimum it exports */
#define OBD_DATA_CACHE_H

/* Provide a minimal stub that satisfies vehicle_profiles.h's #include chain
   without bringing in FreeRTOS or esp_log. */
typedef struct { float min_ratio; float max_ratio; enGear gear; } gear_ratio_range_t;
#define VEHICLE_MAX_GEARS 9

typedef struct {
    float rpm;
    float speed_kmh;
    float coolant_temp_c;
    float oil_temp_c;
    float tps_pct;
    float load_pct;
    float intake_temp_c;
    float boost_kpa;
    int   gear;
    float battery_v;
    float oil_pressure_bar;
    float afr;
} obd_live_t;

/* Silence esp_log references */
#define ESP_LOGI(tag, ...)
#define ESP_LOGW(tag, ...)
#define ESP_LOGE(tag, ...)
#define ESP_LOGD(tag, ...)
#define ESP_LOG_LEVEL_LOCAL(level, tag, ...)

/* vehicle_profiles.c references calculate_gear (defined in obd_data_cache.c).
   Provide a stub. */
static inline enGear calculate_gear(float rpm, float speed){ (void)rpm;(void)speed; return GEAR_N; }

/* Now include the actual firmware source files */
#include "app_obd_dsp/vehicle_custom_config.h"
#include "app_obd_dsp/vehicle_profiles.h"

/* vehicle_profiles.c pulls in the tables; include it directly so the
   s_profiles[] array and s_vehicle_overrides[] are in this translation unit. */
/* rebuild_gear_ranges and vehicle_profile_calc_constant reference
   calculate_gear and math; provide a minimal version. */
#include <math.h>

/* Include just the data tables, not the full .c (which has a real
   rebuild_gear_ranges that uses s_gear_ranges).  We re-implement what
   we need inline below. */
/* Instead of including vehicle_profiles.c directly, copy the two tables
   we need via the header's extern declarations, but that doesn't work for
   a static array.  The cleanest approach: include the .c with a guard that
   skips the non-data parts. */
#define DUMP_LEGACY_SKIP_IMPLS
#include "app_obd_dsp/vehicle_profiles.c"

/* ------------------------------------------------------------------ */
/*  JSON emission helpers                                               */
/* ------------------------------------------------------------------ */

static void jstr(const char *key, const char *val, bool last)
{
    if (!val || val[0] == '\0') {
        printf("    \"%s\": null%s\n", key, last ? "" : ",");
        return;
    }
    printf("    \"%s\": \"", key);
    for (const char *p = val; *p; p++) {
        if (*p == '"' || *p == '\\') putchar('\\');
        putchar(*p);
    }
    printf("\"%s\n", last ? "" : ",");
}

static void jfloat(const char *key, float val, bool last)
{
    printf("    \"%s\": %.6g%s\n", key, (double)val, last ? "" : ",");
}

static void jint(const char *key, int val, bool last)
{
    printf("    \"%s\": %d%s\n", key, val, last ? "" : ",");
}

static void jbool(const char *key, bool val, bool last)
{
    printf("    \"%s\": %s%s\n", key, val ? "true" : "false", last ? "" : ",");
}

static void jhex(const char *key, uint16_t val, bool last)
{
    if (val == 0) {
        printf("    \"%s\": null%s\n", key, last ? "" : ",");
    } else {
        printf("    \"%s\": \"0x%04X\"%s\n", key, (unsigned)val, last ? "" : ",");
    }
}

/* Emit the oil_formula_t as a JSON object (or null if pointer is NULL) */
static void emit_oil_formula(const char *key, const oil_formula_t *f, bool last)
{
    if (!f) {
        printf("    \"%s\": null%s\n", key, last ? "" : ",");
        return;
    }
    const char *type_str = (f->type == OIL_STD_PID) ? "std_pid"
                         : (f->type == OIL_UDS_22)   ? "uds_22"
                                                      : "special";
    printf("    \"%s\": { \"type\": \"%s\",", key, type_str);
    printf(" \"pid\": \"");
    for (int i = 0; i < f->pid_len; i++) printf("%02X%s", f->pid[i], i+1<f->pid_len?" ":"");
    printf("\",");
    printf(" \"pid_len\": %d,", f->pid_len);
    printf(" \"resp_byte\": %d,", f->resp_byte);
    printf(" \"resp_bytes\": %d,", f->resp_bytes);
    printf(" \"scale\": %.8g,", (double)f->scale);
    printf(" \"offset\": %.8g,", (double)f->offset);
    printf(" \"special_id\": %d }", f->special_id);
    printf("%s\n", last ? "" : ",");
}

/* Emit CAN rules array (or empty array if none) */
static void emit_can_rules(const can_rule_t *rules, uint8_t count)
{
    printf("    \"can_rules\": [");
    for (int i = 0; i < count; i++) {
        const can_rule_t *r = &rules[i];
        const char *ch = (r->channel == CH_RPM)      ? "rpm"
                       : (r->channel == CH_SPEED)    ? "speed"
                       : (r->channel == CH_OIL_TEMP) ? "oil_temp"
                       : (r->channel == CH_COOLANT)  ? "coolant"
                       : (r->channel == CH_TPS)      ? "tps"
                       : (r->channel == CH_LOAD)     ? "load"
                       : (r->channel == CH_INTAKE)   ? "intake"
                       : (r->channel == CH_BOOST)    ? "boost"
                       : (r->channel == CH_GEAR)     ? "gear"
                                                     : "unknown";
        printf("%s\n      { \"can_id\": \"0x%03X\","
               " \"bit_off\": %d, \"bit_len\": %d,"
               " \"scale\": %.8g, \"offset\": %.8g,"
               " \"channel\": \"%s\" }",
               i ? "," : "",
               (unsigned)r->can_id, r->bit_off, r->bit_len,
               (double)r->scale, (double)r->offset, ch);
    }
    printf("\n    ],\n");
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    uint8_t count = 0;
    const vehicle_profile_t *profiles = vehicle_profile_get_all(&count);

    printf("[\n");
    for (int i = 0; i < count; i++) {
        const vehicle_profile_t *p = &profiles[i];
        const vehicle_override_t *ov = vehicle_find_override(p->name);

        printf("  {\n");
        jstr("name", p->name, false);

        /* Drivetrain */
        jfloat("final_drive_ratio", p->final_drive_ratio, false);
        jfloat("tire_rolling_radius_m", p->tire_rolling_radius_m, false);
        jint("gear_count", p->gear_count, false);

        /* Gear ratios — index 0 is neutral placeholder */
        printf("    \"gear_ratios\": [");
        for (int g = 0; g <= p->gear_count; g++) {
            printf("%s%.6g", g ? ", " : "", (double)p->gear_ratios[g]);
        }
        printf("],\n");
        jfloat("gear_tolerance", p->gear_tolerance, false);

        /* ELM327 protocol settings */
        jint("forced_protocol", p->forced_protocol, false);
        jbool("obd_functional_addr", p->obd_functional_addr, false);
        jbool("obd_29bit_functional", p->obd_29bit_functional, false);
        jint("obd_timeout", p->obd_timeout, false);
        jint("poll_gap_ms", p->poll_gap_ms, false);
        jbool("has_boost", p->has_boost, false);
        jbool("can_broadcast_mode", p->can_broadcast_mode, false);
        jhex("obd_gear_did", p->obd_gear_did, false);
        jhex("obd_oil_pressure_did", p->obd_oil_pressure_did, false);

        /* Legacy oil temp strategy enums (informational) */
        printf("    \"legacy_oil_strategy\": { "
               "\"primary\": %d, \"secondary\": %d,"
               " \"tertiary\": %d, \"quaternary\": %d },\n",
               (int)p->oil_temp_strategy.primary,
               (int)p->oil_temp_strategy.secondary,
               (int)p->oil_temp_strategy.tertiary,
               (int)p->oil_temp_strategy.quaternary);

        /* Override data (null fields when no override) */
        if (ov) {
            emit_can_rules(ov->can_rules ? ov->can_rules : NULL,
                           ov->can_rules ? ov->can_rule_count : 0);
            emit_oil_formula("oil_primary",   ov->oil_primary,   false);
            emit_oil_formula("oil_secondary", ov->oil_secondary, false);
            jstr("uds_header_cmd",          ov->uds_header_cmd,          false);
            jstr("obd_gear_header_cmd",     ov->obd_gear_header_cmd,     false);
            jstr("obd_gear_rx_filter_cmd",  ov->obd_gear_rx_filter_cmd,  false);
            jstr("obd_gear_raw_frame",      ov->obd_gear_raw_frame,      true);
        } else {
            printf("    \"can_rules\": [],\n");
            printf("    \"oil_primary\": null,\n");
            printf("    \"oil_secondary\": null,\n");
            printf("    \"uds_header_cmd\": null,\n");
            printf("    \"obd_gear_header_cmd\": null,\n");
            printf("    \"obd_gear_rx_filter_cmd\": null,\n");
            printf("    \"obd_gear_raw_frame\": null\n");
        }

        printf("  }%s\n", i + 1 < count ? "," : "");
    }
    printf("]\n");
    return 0;
}
