#pragma once
/*
 * vp_validate — device-side validator for "skygauge.vehicle/1" profiles.
 *
 * The device always re-validates an imported profile, whatever channel it
 * came through (SD drop, device web page, tools). Validation resolves omitted
 * standard OBD2 signals before checking limits/dependencies, without modifying
 * caller-owned JSON. Pure C on top of cJSON;
 * builds on the host for tools/CI (test/host/vehicle).
 *
 * Problems are reported as {code, path, msg}. Error codes (shared with the
 * HTTP API, IMPORT.LOG and the UI):
 *
 *   E_NOMEM       cannot allocate temporary JSON for OBD2 default expansion
 *   E_SIZE        profile text larger than VP_MAX_JSON_BYTES
 *   E_SCHEMA      not JSON / wrong type / missing field / unknown enum value /
 *                 unsupported schema major / inconsistent fields
 *   E_LIMIT       counts or sizes over the limits below
 *   E_NAME        malformed id, name or signal id
 *   E_SIGNAL      obd.* id not in the signal registry, duplicate signal,
 *                 id outside obd.* / veh.*
 *   E_SERVICE     request service not whitelisted (only 01, 09, 21, 22) or
 *                 wrong request length for the service
 *   E_AT          AT command not whitelisted (bus.init) or AT-producing field
 *                 (bus.proto, bus.atst) out of range
 *   E_HDR         header / receive filter / CAN id / extended address format,
 *                 functional addressing used for 21/22
 *   E_RAWFRAME    raw frame is not an ISO-TP single frame, or is ambiguous
 *                 with a forbidden interpretation
 *   E_RPN_OP      malformed or unknown RPN opcode
 *   E_RPN_STACK   RPN stack underflow / overflow / not exactly one result
 *   E_RPN_INDEX   RPN byte index, bit field, X or LUT index out of range
 *   E_RPN_CONST   RPN constant not finite, too large, or division by const 0
 *   E_RATE        poll/monitor timing outside the rate limits
 *   W_UNKNOWN     (warning) unknown key, ignored
 *   W_UNUSED      (warning) declared but unused element
 *
 * Warnings never make a profile invalid.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP_SCHEMA_PREFIX      "skygauge.vehicle/"
#define VP_SCHEMA_MAJOR       1

#define VP_MAX_JSON_BYTES     16384
#define VP_MAX_SIGNALS        48
#define VP_MAX_VEH_SIGNALS    32
#define VP_MAX_SRC_PER_SIGNAL 4
#define VP_MAX_SOURCES        96
#define VP_MAX_REQUESTS       32
#define VP_MAX_CAN_RULES      16
#define VP_MAX_LUTS           8
#define VP_MAX_LUT_ENTRIES    16
#define VP_MAX_GEARS          8
#define VP_MAX_INIT_CMDS      16
#define VP_MAX_REQ_COMMANDS   4     /* APK request Before/AfterCommands per side */
#define VP_MAX_DATA_LEN       64    /* response data bytes addressable by a formula */
#define VP_MAX_EVERY_SIGNALS  2     /* rate "every" is inserted after every slot */
#define VP_MAX_REQ_RATE_HZ    100   /* total request rate */
#define VP_MIN_PERIOD_MS      20    /* per-signal period_ms lower bound */
#define VP_ID_MAX_LEN         31
#define VP_NAME_MAX_LEN       23
#define VP_UNIT_MAX_LEN       7
#define VP_AT_MAX_LEN         15    /* normalized AT command, without CR */

typedef enum {
    VP_SEV_ERROR = 0,
    VP_SEV_WARN  = 1,
} vp_severity_t;

typedef struct {
    const char *code;       /* static string, e.g. "E_SERVICE" */
    uint8_t     severity;   /* vp_severity_t */
    char        path[48];   /* e.g. "signals[3].src[0].req" */
    char        msg[80];
} vp_issue_t;

/* Caller-owned issue list. Counters keep counting after the array is full. */
typedef struct {
    vp_issue_t *items;
    size_t      cap;
    size_t      count;      /* entries stored in items */
    size_t      n_errors;   /* total errors seen */
    size_t      n_warnings; /* total warnings seen */
} vp_issues_t;

void vp_issues_init(vp_issues_t *l, vp_issue_t *storage, size_t cap);
void vp_issue_add(vp_issues_t *l, const char *code, vp_severity_t sev,
                  const char *path, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

/*
 * One entry of the signal registry (contracts/signals.json). Fields after
 * num_id are optional facts (0 / NULL = unknown); the validator itself only
 * needs key and num_id.
 */
typedef struct {
    const char *key;          /* "obd.rpm" */
    uint16_t    num_id;       /* registry numeric id, used by the compiler */
    int8_t      editor_index; /* editor DATA_SOURCES index, -1 = none */
    uint8_t     n_units;      /* number of accepted profile units */
    int32_t     scale;        /* internal units per engineering unit */
    int32_t     min;          /* plausibility range, internal units */
    int32_t     max;
    const char *unit_default; /* profile_unit_default; the compiler stores it
                                 in vp_signal_t.unit when "unit" is absent */
} vp_sig_def_t;

typedef struct {
    const vp_sig_def_t *registry;   /* obd.* signals a profile may provide */
    size_t              n_registry;
    size_t              max_json_bytes;   /* 0 = VP_MAX_JSON_BYTES */
    /*
     * Optional: is unit a profile unit of registry[reg_index]? A profile
     * "unit" on an obd.* signal that fails this check is E_SCHEMA. NULL =
     * any printable unit is accepted.
     */
    bool (*unit_ok)(size_t reg_index, const char *unit);
} vp_validate_cfg_t;

/*
 * The firmware configuration: every obd.* entry of the generated signal
 * registry (app_obd_dsp/signal_registry.h) with its scale, range, editor
 * index and default unit, the obd.* unit check against profile_units, and
 * max_json_bytes = VP_MAX_JSON_BYTES. Built on first call from const data;
 * not thread-safe on that first call (call it once at init). Never NULL.
 */
const vp_validate_cfg_t *vp_validate_default_cfg(void);

/*
 * Validate a parsed profile. Returns the number of errors (0 = valid).
 * out may be NULL when only the verdict matters.
 */
struct cJSON;
size_t vp_validate(const struct cJSON *root, const vp_validate_cfg_t *cfg, vp_issues_t *out);

/* Parse (cJSON_ParseWithLength, no NUL needed) and validate. */
size_t vp_validate_json(const char *text, size_t len, const vp_validate_cfg_t *cfg,
                        vp_issues_t *out);

typedef enum {
    VP_AT_CTX_ENGINE = 0,   /* commands the firmware itself generates */
    VP_AT_CTX_INIT   = 1,   /* extra commands listed in a profile's bus.init */
    VP_AT_CTX_REQUEST = 2,  /* safe per-request BeforeCommands/AfterCommands */
} vp_at_ctx_t;

/*
 * AT whitelist: SH (whitelisted headers), CRA (reset or whitelisted
 * filter), CEA (off or one byte), CAF0, CAF1, ST hh (02..FF), SP h / SP Ah
 * (h = 0-9, A-C). VP_AT_CTX_ENGINE additionally accepts the fixed commands
 * the firmware itself sends: Z, E0, L0, S1, S0, M0, AL, H0, H1, AT1, I, RV
 * (link setup and vehicle check) and MA, AR (monitor window). Everything else (PP, BRD,
 * BRT, WS, LP, MR, MT, SD, KW, ...) and any non-AT command (STN
 * "ST..." extensions) is refused in both contexts. In VP_AT_CTX_INIT, reset/default
 * commands Z, D, D0, common setup commands, and bounded legacy init commands
 * (IB10, SW<hh>, IIA[<hh>], SI, FI, WM<hh...>, TAF1, CER) are allowed so a
 * translated Car Scanner InitSequence can retain its original ordering; CAF0 and
 * CEA remain refused because they change the framing of every later request. The
 * normalized command (upper case, no spaces/CR) is written to norm when it
 * is non-NULL. *why gets a short reason on refusal.
 */
bool vp_at_check(const char *cmd, vp_at_ctx_t ctx, char *norm, size_t norm_cap,
                 const char **why);

/* One narrowly scoped non-AT initialization frame used by Nissan Consult II
 * profiles translated from Car Scanner. It is valid only in bus.init. */
bool vp_init_raw_command(const char *cmd);

/* Profile-only timing controls accepted in bus.init.  They are represented
 * as strings to preserve the existing JSON format, but never go to ELM327. */
bool vp_init_wait_ms(const char *token, uint16_t *out_ms);

#ifdef __cplusplus
}
#endif
