#pragma once
/*
 * vp_profile — compiled runtime representation of a "skygauge.vehicle/1"
 * profile.
 *
 * vp_compile() turns a validated JSON profile into one contiguous,
 * position-independent blob allocated through an injected allocator (the
 * firmware passes a heap_caps_calloc(MALLOC_CAP_SPIRAM) wrapper). Tables are
 * addressed by offsets from the blob start, so the same bytes work from
 * PSRAM, from NVS or as a const array in flash (vp_builtin_generic.c).
 *
 * Layout: [vp_profile_t][init][requests][can rules][signals][sources]
 *         [code][luts][lut entries], every table 4-byte aligned. All fields
 *         are fixed-width little-endian with explicit padding; the sizes are
 *         asserted below so a host-generated blob matches the ESP32-S3 ABI.
 *
 * vp_engine (vp_engine.h) runs this structure against the ELM327; that is
 * not part of this module.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "vp_rpn.h"
#include "vp_validate.h"
#include "vp_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VP_PROFILE_MAGIC        0x31525056u   /* "VPR1" */
#define VP_PROFILE_VERSION      1
#define VP_DEFAULT_PROTO        0             /* ATSP0 / adapter OBD-II auto-detect */
#define VP_DEFAULT_HDR          0x7E0u
#define VP_DEFAULT_GAP_MS       30            /* legacy OBD_POLL_SLOT_GAP_MS */
#define VP_DEFAULT_ATST         0x19          /* legacy default when unset */
#define VP_DEFAULT_FAIL_AFTER   5
#define VP_DEFAULT_SLOW_DIV     4
#define VP_PROFILE_CMD_CAP      20
#define VP_MIN_REQ_INTERVAL_MS  (1000 / VP_MAX_REQ_RATE_HZ)
#define VP_VEH_NUM_ID_BASE      0x0100u

/* vp_profile_t.requires */
#define VP_REQ_ATMA   0x01u
#define VP_REQ_ATCEA  0x02u

/* vp_profile_t.mon_mode */
enum { VP_MON_NONE = 0, VP_MON_PROBE = 1, VP_MON_CONTINUOUS = 2 };

/* vp_request_t.flags */
#define VP_REQ_F_RAW    0x01u   /* tx is a raw CAN payload: send between ATCAF0/ATCAF1 */
#define VP_REQ_F_MULTI  0x02u   /* response may span several frames */
#define VP_REQ_F_CEA    0x04u   /* send with ATCEA <cea>, restore ATCEA after */
#define VP_REQ_F_FUNC   0x08u   /* hdr is a functional (broadcast) address */
#define VP_REQ_F_RX     0x10u   /* set ATCRA <rx> before, ATCRA after */

/* vp_request_t.rsv: internal request identity retained from profile input. */
#define VP_REQ_RSV_HDR_EXPLICIT 0x01u /* JSON source supplied hdr explicitly */

/* vp_source_t.kind / flags */
enum { VP_SRC_REQ = 1, VP_SRC_CAN = 2 };
#define VP_SRC_F_FROM_END 0x01u   /* formula data = last byte_from_end bytes of the response */
#define VP_SRC_F_HOLD     0x02u   /* jump filter: hold last value (hold_*) */
#define VP_SRC_F_VALID    0x04u   /* reject samples outside [valid_lo, valid_hi] */

/* vp_signal_t.rate */
enum { VP_RATE_ROUND = 0, VP_RATE_EVERY = 1, VP_RATE_SLOW = 2, VP_RATE_ON_DEMAND = 3 };
/* vp_signal_t.filter */
enum { VP_FILTER_NONE = 0, VP_FILTER_TEMP_HYST = 1, VP_FILTER_EMA = 2 };
/* vp_signal_t.derive / fallback */
enum { VP_DERIVE_NONE = 0, VP_DERIVE_RATIO = 1 };
/* vp_signal_t.flags */
#define VP_SIG_F_VALID     0x0001u  /* reject final values outside [valid_lo, valid_hi] */
#define VP_SIG_F_CLAMP     0x0002u  /* clamp final value to [clamp_lo, clamp_hi] */
#define VP_SIG_F_ZERO_LE   0x0004u  /* value <= zero_le -> 0 */
#define VP_SIG_F_ZERO_RPM  0x0008u  /* obd.rpm < zero_if_rpm_lt -> 0 */
#define VP_SIG_F_VEH       0x0010u  /* veh.* signal */

typedef struct {
    char cmd[16];                 /* normalized AT command, no CR, NUL-terminated */
} vp_at_cmd_t;

typedef struct {
    uint32_t hdr;                 /* effective tx header (VP_ID_29BIT for 29-bit) */
    uint32_t rx;                  /* ATCRA filter, valid with VP_REQ_F_RX */
    uint8_t  flags;               /* VP_REQ_F_* */
    uint8_t  cea;                 /* extended address, valid with VP_REQ_F_CEA */
    uint8_t  tx_len;
    uint8_t  expect_len;
    uint8_t  tx[8];               /* service + parameters, or raw CAN payload */
    uint8_t  expect[8];           /* positive response prefix; data bytes follow */
    uint8_t  service;             /* request service (embedded one for raw frames) */
    uint8_t  before_count;        /* APK BeforeCommands, executed before tx */
    uint8_t  after_count;         /* APK AfterCommands, deferred to next hdr change */
    uint8_t  rsv;
    vp_at_cmd_t before[VP_MAX_REQ_COMMANDS];
    vp_at_cmd_t after[VP_MAX_REQ_COMMANDS];
} vp_request_t;

typedef struct {
    uint32_t can_id;              /* VP_ID_29BIT for 29-bit */
    uint16_t src;                 /* source index */
    uint16_t sig;                 /* signal index */
} vp_can_rule_t;

typedef struct {
    uint8_t  kind;                /* VP_SRC_REQ / VP_SRC_CAN */
    uint8_t  flags;               /* VP_SRC_F_* */
    uint8_t  len;                 /* data bytes the formula may address */
    uint8_t  byte_from_end;       /* with VP_SRC_F_FROM_END */
    uint16_t ref;                 /* request index (REQ) or can rule index (CAN) */
    uint16_t sig;                 /* owning signal index */
    uint16_t code_off;            /* first instruction in the code table */
    uint8_t  code_len;
    uint8_t  fail_after;          /* consecutive failures before the next source */
    uint16_t raw_start;           /* the source's X field (informational, already in code) */
    uint8_t  raw_bits;
    uint8_t  raw_flags;
    float    valid_lo;
    float    valid_hi;
    float    hold_delta;          /* with VP_SRC_F_HOLD: max plausible jump */
    uint16_t hold_gap_ms;         /* a gap longer than this resets the hold */
    uint8_t  hold_max;            /* max consecutive held samples */
    uint8_t  rsv;
} vp_source_t;

typedef struct {
    char     key[32];             /* "obd.rpm" / "veh.boost_target" */
    char     name[24];            /* display name (veh.*), may be empty */
    char     unit[8];             /* informational, may be empty */
    uint16_t num_id;              /* registry id; veh.* get VP_VEH_NUM_ID_BASE + n */
    uint16_t src_first;
    uint8_t  src_count;
    uint8_t  rate;                /* VP_RATE_* */
    uint8_t  slow_div;            /* VP_RATE_SLOW: poll every slow_div rounds */
    uint8_t  filter;              /* VP_FILTER_* */
    uint8_t  derive;              /* VP_DERIVE_*: computed, no sources */
    uint8_t  fallback;            /* VP_DERIVE_*: used after all sources failed */
    uint16_t flags;               /* VP_SIG_F_* */
    uint16_t period_ms;           /* minimum poll interval, 0 = none */
    uint16_t zero_if_rpm_lt;
    float    valid_lo;
    float    valid_hi;
    float    clamp_lo;
    float    clamp_hi;
    float    zero_le;
} vp_signal_t;

typedef struct {
    uint32_t magic;               /* VP_PROFILE_MAGIC */
    uint16_t version;             /* VP_PROFILE_VERSION */
    uint16_t header_size;         /* sizeof(vp_profile_t) */
    uint32_t total_size;          /* whole blob */
    uint32_t crc32;               /* vp_crc32 over the blob with this field = 0 */
    char     id[32];
    char     name[24];
    uint16_t rev;
    uint8_t  requires;            /* VP_REQ_ATMA | VP_REQ_ATCEA */
    uint8_t  proto;               /* ATSP value 0..12 (0 = auto) */
    uint32_t hdr;                 /* default tx header */
    uint8_t  hdr_functional;
    uint8_t  atst;                /* ATST value */
    uint16_t gap_ms;              /* slot gap */
    float    final_drive;
    float    tire_m;
    float    gear_tol;
    float    speed_scale;         /* 1.0 when unset */
    float    gears[VP_MAX_GEARS];
    uint8_t  gear_count;
    uint8_t  mon_mode;            /* VP_MON_* */
    uint16_t mon_every_ms;
    uint16_t mon_window_ms;
    uint16_t mon_stale_ms;
    uint16_t n_init;
    uint16_t n_req;
    uint16_t n_can;
    uint16_t n_sig;
    uint16_t n_src;
    uint16_t n_code;
    uint16_t n_lut;
    uint16_t n_lut_entry;
    uint32_t off_init;
    uint32_t off_req;
    uint32_t off_can;
    uint32_t off_sig;
    uint32_t off_src;
    uint32_t off_code;
    uint32_t off_lut;
    uint32_t off_lut_entry;
    uint16_t min_req_ms;          /* scheduler lower bound between request starts */
    char     check_connection[VP_PROFILE_CMD_CAP]; /* default 0100 */
    char     tester_present[VP_PROFILE_CMD_CAP];   /* optional OBD tester-present command */
    uint16_t tester_present_every_ms;              /* 0 = disabled */
} vp_profile_t;

#define VP__ASSERT(name, cond) typedef char vp__assert_##name[(cond) ? 1 : -1]
VP__ASSERT(insn,    sizeof(vp_rpn_insn_t) == 8);
VP__ASSERT(lut,     sizeof(vp_rpn_lut_t) == 4);
VP__ASSERT(lut_e,   sizeof(vp_rpn_lut_entry_t) == 12);
VP__ASSERT(at,      sizeof(vp_at_cmd_t) == 16);
VP__ASSERT(req,     sizeof(vp_request_t) == 160);
VP__ASSERT(can,     sizeof(vp_can_rule_t) == 8);
VP__ASSERT(src,     sizeof(vp_source_t) == 32);
VP__ASSERT(sig,     sizeof(vp_signal_t) == 100);
VP__ASSERT(profile, sizeof(vp_profile_t) == 232);

typedef struct {
    void *(*alloc)(void *ctx, size_t size);   /* 4-byte aligned; need not be zeroed */
    void  (*free)(void *ctx, void *ptr);
    void  *ctx;
} vp_allocator_t;

typedef enum {
    VP_COMPILE_OK = 0,
    VP_COMPILE_ERR_ARG,
    VP_COMPILE_ERR_PARSE,       /* text is not JSON */
    VP_COMPILE_ERR_INVALID,     /* validation failed, see issues */
    VP_COMPILE_ERR_NOMEM,
    VP_COMPILE_ERR_INTERNAL,    /* validator/compiler disagreement */
} vp_compile_err_t;

/*
 * Expand omitted standard OBD2 signals, validate and compile. Explicit signal
 * declarations replace defaults; disabled entries are omitted from the blob.
 * The caller-owned JSON is unchanged. On success *out is one allocation from alloc; free
 * it with vp_profile_free(). issues may be NULL.
 */
struct cJSON;
vp_compile_err_t vp_compile(const struct cJSON *root, const vp_validate_cfg_t *cfg,
                            const vp_allocator_t *alloc, vp_profile_t **out,
                            vp_issues_t *issues);

vp_compile_err_t vp_compile_json(const char *text, size_t len, const vp_validate_cfg_t *cfg,
                                 const vp_allocator_t *alloc, vp_profile_t **out,
                                 vp_issues_t *issues);

void vp_profile_free(vp_profile_t *p, const vp_allocator_t *alloc);

/*
 * Structural check of a blob from storage or flash: magic, version, size,
 * CRC, table bounds, cross references, and vp_rpn_verify() of every
 * formula. Returns true when the blob is safe to use.
 */
bool vp_profile_check(const void *blob, size_t len);

/* Table accessors. */
#define VP__TABLE(p, off, type) ((const type *)(const void *)((const uint8_t *)(p) + (p)->off))

static inline const vp_at_cmd_t *vp_profile_init(const vp_profile_t *p)
{ return VP__TABLE(p, off_init, vp_at_cmd_t); }
static inline const vp_request_t *vp_profile_requests(const vp_profile_t *p)
{ return VP__TABLE(p, off_req, vp_request_t); }
static inline const vp_can_rule_t *vp_profile_can_rules(const vp_profile_t *p)
{ return VP__TABLE(p, off_can, vp_can_rule_t); }
static inline const vp_signal_t *vp_profile_signals(const vp_profile_t *p)
{ return VP__TABLE(p, off_sig, vp_signal_t); }
static inline const vp_source_t *vp_profile_sources(const vp_profile_t *p)
{ return VP__TABLE(p, off_src, vp_source_t); }
static inline const vp_rpn_insn_t *vp_profile_code(const vp_profile_t *p)
{ return VP__TABLE(p, off_code, vp_rpn_insn_t); }
static inline const vp_rpn_lut_t *vp_profile_luts(const vp_profile_t *p)
{ return VP__TABLE(p, off_lut, vp_rpn_lut_t); }
static inline const vp_rpn_lut_entry_t *vp_profile_lut_entries(const vp_profile_t *p)
{ return VP__TABLE(p, off_lut_entry, vp_rpn_lut_entry_t); }

/* LUT environment for vp_rpn_eval() of this profile's formulas. */
static inline vp_rpn_env_t vp_profile_rpn_env(const vp_profile_t *p)
{
    vp_rpn_env_t env;
    env.luts = vp_profile_luts(p);
    env.entries = vp_profile_lut_entries(p);
    env.n_luts = p->n_lut;
    env.n_entries = p->n_lut_entry;
    return env;
}

/* Find a signal by key; returns its index or -1. */
int vp_profile_find_signal(const vp_profile_t *p, const char *key);

/*
 * Evaluate source si of profile p over response/frame data. For
 * VP_SRC_F_FROM_END sources pass the whole response: the tail view is
 * selected here. Applies the source's valid range (not hold/filters).
 */
vp_rpn_err_t vp_profile_eval_source(const vp_profile_t *p, uint16_t si,
                                    const uint8_t *data, size_t len, float *out);

#ifdef __cplusplus
}
#endif
