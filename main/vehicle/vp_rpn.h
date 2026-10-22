#pragma once
/*
 * vp_rpn — safe formula executor for vehicle profiles (skygauge.vehicle/1).
 *
 * A formula is a straight-line RPN program: no jumps, no loops, at most
 * VP_RPN_MAX_INSNS instructions, evaluation stack depth at most
 * VP_RPN_MAX_STACK. Values are float32. A program must pass vp_rpn_verify()
 * before vp_rpn_eval() runs it; vp_rpn_eval() still re-checks everything at
 * runtime, so an unverified program can never read out of bounds.
 *
 * Profiles never carry infix expressions for the device. Authoring tools
 * compile the "expr" notation (A..H, X) into the JSON form below with
 * tools/vehicles/expr2rpn.py; the device only accepts "rpn".
 *
 * ---------------------------------------------------------------------------
 * JSON spelling (a program is a JSON array; each element is one opcode)
 * ---------------------------------------------------------------------------
 *
 *  Opcode      JSON                        Stack            Semantics
 *  ----------  --------------------------  ---------------  -------------------------------
 *  PUSH_BYTE   ["A", i]                    -> d[i]          Data byte i (0-based, unsigned).
 *                                                           i must be < the source's declared
 *                                                           data length (static check).
 *                                                           Infix: A=["A",0] .. H=["A",7].
 *  PUSH_RAW    ["R", start, bits]          -> field         Bit field of the source data;
 *              ["R", start, bits, opt...]                   opt is "le"|"be" (default "le")
 *                                                           and/or "u"|"s" (default "u").
 *                                                           bits 1..32; start+bits must fit in
 *                                                           the declared data length.
 *                                                           "le": bit k is d[k/8] bit (k%8), the
 *                                                           first bit is the LSB (Intel; this is
 *                                                           the legacy can_extract_bits_le()).
 *                                                           "be": bit k is d[k/8] bit (7-k%8), the
 *                                                           first bit is the MSB (network order:
 *                                                           ["R",16,16,"be"] == d[2]*256+d[3]).
 *                                                           "s": two's-complement sign extension.
 *              ["X"]                       -> field         Shorthand for the source's own raw
 *                                                           field, resolved when the program is
 *                                                           decoded: a "can" source's bit/bits/
 *                                                           endian/signed, or the byte selected by
 *                                                           a multiframe source's byte_from_end.
 *                                                           Not allowed in other sources.
 *  PUSH_CONST  ["k", c]                    -> c             c finite and |c| <= 1e7.
 *  ADD         "+"                         a b -> a+b
 *  SUB         "-"                         a b -> a-b
 *  MUL         "*"                         a b -> a*b
 *  DIV         "/"                         a b -> a/b       b == 0 -> invalid. A constant 0
 *                                                           directly before "/" is rejected
 *                                                           statically.
 *  NEG         "neg"                       a -> -a
 *  MIN         "min"                       a b -> min(a,b)
 *  MAX         "max"                       a b -> max(a,b)
 *  CLAMP       "clamp"                     x lo hi -> y     y = min(max(x,lo),hi); lo > hi -> invalid
 *  AND         "&"                         a b -> a&b       Integer ops: each operand is truncated
 *  OR          "|"                         a b -> a|b       toward zero and must lie in
 *  SHR         ">>"                        a n -> a>>n      [-2^31, 2^32-1] (else invalid); it is
 *  SHL         "<<"                        a n -> a<<n      then used as a 32-bit two's-complement
 *                                                           pattern and the result is pushed as an
 *                                                           unsigned value. Shifts are logical,
 *                                                           n must be 0..31, bits shifted past
 *                                                           bit 31 are dropped.
 *  SEXT8       "sext8"                     a -> s           Low 8 bits as signed (-128..127).
 *  SEXT16      "sext16"                    a -> s           Low 16 bits as signed.
 *  LUT         ["lut", n]                  x -> y           Table n of the profile's "luts": the
 *                                                           first entry with lo <= x <= hi yields
 *                                                           its out value; no match -> invalid.
 *
 *  Example (RPM, (A*256+B)/4):
 *      [["A",0],["k",256],"*",["A",1],"+",["k",4],"/"]
 *
 *  Static verification (vp_rpn_verify): 1..32 instructions, simulated stack
 *  never underflows, never exceeds 8, ends with exactly one value; byte
 *  indexes and bit fields inside the declared length; LUT index below the
 *  profile's table count; constants finite, |c| <= 1e7.
 *
 *  Runtime (vp_rpn_eval): any NaN/Inf intermediate or result, division by
 *  zero, integer-op range error, out-of-range data access or LUT miss makes
 *  the sample invalid (non-VP_RPN_OK status, *out untouched).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP_RPN_MAX_INSNS   32
#define VP_RPN_MAX_STACK   8
#define VP_RPN_MAX_CONST   1e7
#define VP_RPN_MAX_RAWBITS 32

/* Opcode values are stored in compiled profile blobs: append only. */
typedef enum {
    VP_OP_INVALID    = 0,
    VP_OP_PUSH_BYTE  = 1,
    VP_OP_PUSH_RAW   = 2,
    VP_OP_PUSH_CONST = 3,
    VP_OP_ADD        = 4,
    VP_OP_SUB        = 5,
    VP_OP_MUL        = 6,
    VP_OP_DIV        = 7,
    VP_OP_NEG        = 8,
    VP_OP_MIN        = 9,
    VP_OP_MAX        = 10,
    VP_OP_CLAMP      = 11,
    VP_OP_AND        = 12,
    VP_OP_OR         = 13,
    VP_OP_SHR        = 14,
    VP_OP_SHL        = 15,
    VP_OP_SEXT8      = 16,
    VP_OP_SEXT16     = 17,
    VP_OP_LUT        = 18,
    VP_OP_COUNT
} vp_op_t;

/* PUSH_RAW flags */
#define VP_RAW_BE      0x01u
#define VP_RAW_SIGNED  0x02u

/*
 * One instruction, 8 bytes, no padding (stored in compiled blobs).
 *   PUSH_BYTE : u.arg = byte index
 *   PUSH_RAW  : u.arg = start bit, bits = bit count, flags = VP_RAW_*
 *   PUSH_CONST: u.k   = constant
 *   LUT       : u.arg = table index
 */
typedef struct {
    uint8_t op;
    uint8_t bits;
    uint8_t flags;
    uint8_t rsv;
    union {
        float    k;
        uint32_t arg;
    } u;
} vp_rpn_insn_t;

/* Raw field used to resolve ["X"] while decoding JSON. */
typedef struct {
    uint16_t start;
    uint8_t  bits;
    uint8_t  flags;
} vp_rpn_raw_t;

/* LUT storage shared by all formulas of a profile. */
typedef struct {
    float lo;
    float hi;
    float out;
} vp_rpn_lut_entry_t;

typedef struct {
    uint16_t first;   /* index into the entry array */
    uint8_t  count;
    uint8_t  rsv;
} vp_rpn_lut_t;

typedef struct {
    const vp_rpn_lut_t       *luts;
    const vp_rpn_lut_entry_t *entries;
    uint16_t                  n_luts;
    uint16_t                  n_entries;
} vp_rpn_env_t;

typedef enum {
    VP_RPN_OK = 0,
    /* static (verify / decode) */
    VP_RPN_ERR_EMPTY,       /* no instructions */
    VP_RPN_ERR_TOO_LONG,    /* more than VP_RPN_MAX_INSNS */
    VP_RPN_ERR_OPCODE,      /* unknown opcode / malformed JSON op */
    VP_RPN_ERR_UNDERFLOW,   /* stack underflow */
    VP_RPN_ERR_OVERFLOW,    /* stack deeper than VP_RPN_MAX_STACK */
    VP_RPN_ERR_RESULT,      /* program does not end with exactly one value */
    VP_RPN_ERR_INDEX,       /* byte index >= declared length */
    VP_RPN_ERR_RAW,         /* bad bit field (bits 0/>32, outside declared length) */
    VP_RPN_ERR_X,           /* ["X"] used where the source has no raw field */
    VP_RPN_ERR_LUT,         /* LUT index >= table count */
    VP_RPN_ERR_CONST,       /* constant not finite or |c| > VP_RPN_MAX_CONST */
    VP_RPN_ERR_DIV0,        /* division by constant zero (static) / by zero (runtime) */
    /* runtime only */
    VP_RPN_ERR_NONFINITE,   /* NaN or Inf produced */
    VP_RPN_ERR_DATA,        /* data shorter than the program needs */
    VP_RPN_ERR_INT,         /* integer-op operand or shift count out of range */
    VP_RPN_ERR_LUT_MISS,    /* value not covered by the table */
    VP_RPN_ERR_CLAMP,       /* CLAMP with lo > hi */
    VP_RPN_ERR_ARG,         /* NULL pointer argument */
    VP_RPN_ERR_RANGE,       /* result outside the source's valid range (profile layer) */
} vp_rpn_err_t;

typedef struct {
    uint16_t data_len;   /* declared data bytes visible to PUSH_BYTE / PUSH_RAW */
    uint16_t n_luts;     /* number of LUT tables in the profile */
} vp_rpn_verify_cfg_t;

/* Stack effect of one opcode; returns false for unknown opcodes. */
bool vp_rpn_op_arity(uint8_t op, uint8_t *pops, uint8_t *pushes);

/* Canonical JSON spelling of an opcode ("+", "neg", "A", "R", "k", "lut" ...). */
const char *vp_rpn_op_name(uint8_t op);

const char *vp_rpn_err_str(vp_rpn_err_t err);

/*
 * Static verification. On failure *err_pc (if non-NULL) is the offending
 * instruction index (n for end-of-program errors).
 */
vp_rpn_err_t vp_rpn_verify(const vp_rpn_insn_t *code, size_t n,
                           const vp_rpn_verify_cfg_t *cfg, size_t *err_pc);

/*
 * Evaluate a program over len data bytes. env may be NULL when the program
 * has no LUT. Returns VP_RPN_OK and stores the result in *out, or an error
 * (the sample is invalid) and leaves *out untouched.
 */
vp_rpn_err_t vp_rpn_eval(const vp_rpn_insn_t *code, size_t n,
                         const uint8_t *data, size_t len,
                         const vp_rpn_env_t *env, float *out);

/* Bit field extraction shared with the profile engine (CAN monitor). */
bool vp_rpn_extract_raw(const uint8_t *data, size_t len, uint16_t start,
                        uint8_t bits, uint8_t flags, int64_t *out);

/*
 * Decode the JSON spelling (a cJSON array) into instructions. x_field
 * resolves ["X"]; pass NULL when the source has no raw field. Only syntax
 * and per-op argument ranges are checked here: run vp_rpn_verify()
 * afterwards. *err_index is the offending array index on failure.
 * Implemented in vp_rpn_json.c (needs cJSON).
 */
struct cJSON;
vp_rpn_err_t vp_rpn_from_json(const struct cJSON *arr, const vp_rpn_raw_t *x_field,
                              vp_rpn_insn_t *out, size_t cap, size_t *n_out,
                              size_t *err_index);

#ifdef __cplusplus
}
#endif
