#include "vp_rpn.h"

#include <math.h>
#include <string.h>

typedef struct {
    uint8_t     pops;
    uint8_t     pushes;
    const char *name;
} op_info_t;

static const op_info_t s_ops[VP_OP_COUNT] = {
    [VP_OP_INVALID]    = { 0, 0, NULL },
    [VP_OP_PUSH_BYTE]  = { 0, 1, "A" },
    [VP_OP_PUSH_RAW]   = { 0, 1, "R" },
    [VP_OP_PUSH_CONST] = { 0, 1, "k" },
    [VP_OP_ADD]        = { 2, 1, "+" },
    [VP_OP_SUB]        = { 2, 1, "-" },
    [VP_OP_MUL]        = { 2, 1, "*" },
    [VP_OP_DIV]        = { 2, 1, "/" },
    [VP_OP_NEG]        = { 1, 1, "neg" },
    [VP_OP_MIN]        = { 2, 1, "min" },
    [VP_OP_MAX]        = { 2, 1, "max" },
    [VP_OP_CLAMP]      = { 3, 1, "clamp" },
    [VP_OP_AND]        = { 2, 1, "&" },
    [VP_OP_OR]         = { 2, 1, "|" },
    [VP_OP_SHR]        = { 2, 1, ">>" },
    [VP_OP_SHL]        = { 2, 1, "<<" },
    [VP_OP_SEXT8]      = { 1, 1, "sext8" },
    [VP_OP_SEXT16]     = { 1, 1, "sext16" },
    [VP_OP_LUT]        = { 1, 1, "lut" },
};

bool vp_rpn_op_arity(uint8_t op, uint8_t *pops, uint8_t *pushes)
{
    if (op == VP_OP_INVALID || op >= VP_OP_COUNT) {
        return false;
    }
    if (pops) *pops = s_ops[op].pops;
    if (pushes) *pushes = s_ops[op].pushes;
    return true;
}

const char *vp_rpn_op_name(uint8_t op)
{
    if (op == VP_OP_INVALID || op >= VP_OP_COUNT) {
        return NULL;
    }
    return s_ops[op].name;
}

const char *vp_rpn_err_str(vp_rpn_err_t err)
{
    switch (err) {
    case VP_RPN_OK:            return "ok";
    case VP_RPN_ERR_EMPTY:     return "empty program";
    case VP_RPN_ERR_TOO_LONG:  return "too many instructions";
    case VP_RPN_ERR_OPCODE:    return "unknown or malformed opcode";
    case VP_RPN_ERR_UNDERFLOW: return "stack underflow";
    case VP_RPN_ERR_OVERFLOW:  return "stack overflow";
    case VP_RPN_ERR_RESULT:    return "must leave exactly one value";
    case VP_RPN_ERR_INDEX:     return "byte index beyond declared length";
    case VP_RPN_ERR_RAW:       return "bit field outside declared length";
    case VP_RPN_ERR_X:         return "X not available in this source";
    case VP_RPN_ERR_LUT:       return "LUT index out of range";
    case VP_RPN_ERR_CONST:     return "constant not finite or too large";
    case VP_RPN_ERR_DIV0:      return "division by zero";
    case VP_RPN_ERR_NONFINITE: return "non-finite value";
    case VP_RPN_ERR_DATA:      return "response too short";
    case VP_RPN_ERR_INT:       return "integer operand out of range";
    case VP_RPN_ERR_LUT_MISS:  return "value not in LUT";
    case VP_RPN_ERR_CLAMP:     return "clamp lo > hi";
    case VP_RPN_ERR_ARG:       return "bad argument";
    case VP_RPN_ERR_RANGE:     return "outside valid range";
    }
    return "?";
}

static bool const_ok(float k)
{
    return isfinite(k) && fabsf(k) <= (float)VP_RPN_MAX_CONST;
}

static vp_rpn_err_t check_operand_static(const vp_rpn_insn_t *in, const vp_rpn_verify_cfg_t *cfg)
{
    switch (in->op) {
    case VP_OP_PUSH_BYTE:
        if (in->u.arg >= cfg->data_len) return VP_RPN_ERR_INDEX;
        break;
    case VP_OP_PUSH_RAW:
        if (in->bits == 0 || in->bits > VP_RPN_MAX_RAWBITS) return VP_RPN_ERR_RAW;
        if ((in->flags & ~(VP_RAW_BE | VP_RAW_SIGNED)) != 0) return VP_RPN_ERR_RAW;
        if ((uint64_t)in->u.arg + in->bits > (uint64_t)cfg->data_len * 8u) return VP_RPN_ERR_RAW;
        break;
    case VP_OP_PUSH_CONST:
        if (!const_ok(in->u.k)) return VP_RPN_ERR_CONST;
        break;
    case VP_OP_LUT:
        if (in->u.arg >= cfg->n_luts) return VP_RPN_ERR_LUT;
        break;
    default:
        break;
    }
    return VP_RPN_OK;
}

vp_rpn_err_t vp_rpn_verify(const vp_rpn_insn_t *code, size_t n,
                           const vp_rpn_verify_cfg_t *cfg, size_t *err_pc)
{
    size_t depth = 0;
    size_t pc;
    vp_rpn_err_t err = VP_RPN_OK;

    if (err_pc) *err_pc = 0;
    if (!cfg || (!code && n)) return VP_RPN_ERR_ARG;
    if (n == 0) return VP_RPN_ERR_EMPTY;
    if (n > VP_RPN_MAX_INSNS) {
        if (err_pc) *err_pc = VP_RPN_MAX_INSNS;
        return VP_RPN_ERR_TOO_LONG;
    }

    for (pc = 0; pc < n; pc++) {
        uint8_t pops, pushes;
        const vp_rpn_insn_t *in = &code[pc];

        if (!vp_rpn_op_arity(in->op, &pops, &pushes)) {
            err = VP_RPN_ERR_OPCODE;
            break;
        }
        err = check_operand_static(in, cfg);
        if (err != VP_RPN_OK) break;
        if (in->op == VP_OP_DIV && pc > 0 &&
            code[pc - 1].op == VP_OP_PUSH_CONST && code[pc - 1].u.k == 0.0f) {
            err = VP_RPN_ERR_DIV0;
            break;
        }
        if (depth < pops) {
            err = VP_RPN_ERR_UNDERFLOW;
            break;
        }
        depth = depth - pops + pushes;
        if (depth > VP_RPN_MAX_STACK) {
            err = VP_RPN_ERR_OVERFLOW;
            break;
        }
    }
    if (err == VP_RPN_OK && depth != 1) {
        err = VP_RPN_ERR_RESULT;
    }
    if (err != VP_RPN_OK && err_pc) *err_pc = pc;
    return err;
}

bool vp_rpn_extract_raw(const uint8_t *data, size_t len, uint16_t start,
                        uint8_t bits, uint8_t flags, int64_t *out)
{
    uint32_t v = 0;
    unsigned byte = start >> 3;
    unsigned offset = start & 7u;
    unsigned remaining = bits;

    if (!data || !out || bits == 0 || bits > 32) return false;
    if (((uint32_t)start + bits + 7u) / 8u > len) return false;

    /* Consume only the bytes touched by the field (at most five). No
     * unaligned word loads or reads beyond the declared payload length. */
    if (flags & VP_RAW_BE) {
        while (remaining) {
            unsigned take = 8u - offset;
            if (take > remaining) take = remaining;
            uint32_t mask = (1u << take) - 1u;
            v = (v << take) | ((data[byte++] >> (8u - offset - take)) & mask);
            remaining -= take;
            offset = 0;
        }
    } else {
        unsigned shift = 0;
        while (remaining) {
            unsigned take = 8u - offset;
            if (take > remaining) take = remaining;
            uint32_t mask = (1u << take) - 1u;
            v |= ((uint32_t)(data[byte++] >> offset) & mask) << shift;
            remaining -= take;
            shift += take;
            offset = 0;
        }
    }
    if (flags & VP_RAW_SIGNED) {
        /* Flip the sign bit, then subtract its weight. This handles 32-bit
         * fields without a data-dependent branch or signed-shift overflow. */
        uint32_t sign = 1u << (bits - 1);
        *out = (int64_t)(v ^ sign) - sign;
    } else {
        *out = (int64_t)v;
    }
    return true;
}

/* Integer-op operand: truncate toward zero, range [-2^31, 2^32-1], 32-bit pattern. */
static bool to_u32(float f, uint32_t *out)
{
    /* Binary32 has no value between -2^31 and -2^31-1. Keep the exact
     * accepted set, then truncate inside the signed/unsigned 32-bit ranges. */
    if (!isfinite(f) || f < -2147483648.0f || f >= 4294967296.0f) {
        return false;
    }
    *out = f < 0.0f ? (uint32_t)(int32_t)f : (uint32_t)f;
    return true;
}

static bool to_shift(float f, uint32_t *out)
{
    if (!isfinite(f) || f <= -1.0f || f >= 32.0f) {
        return false;
    }
    *out = (uint32_t)f;
    return true;
}

static vp_rpn_err_t lut_lookup(const vp_rpn_env_t *env, uint32_t idx, float x, float *y)
{
    const vp_rpn_lut_t *t;
    uint16_t i;

    if (!env || !env->luts || idx >= env->n_luts) return VP_RPN_ERR_LUT;
    t = &env->luts[idx];
    if (!env->entries || (uint32_t)t->first + t->count > env->n_entries) return VP_RPN_ERR_LUT;
    for (i = 0; i < t->count; i++) {
        const vp_rpn_lut_entry_t *e = &env->entries[t->first + i];
        if (x >= e->lo && x <= e->hi) {
            *y = e->out;
            return VP_RPN_OK;
        }
    }
    return VP_RPN_ERR_LUT_MISS;
}

vp_rpn_err_t vp_rpn_eval(const vp_rpn_insn_t *code, size_t n,
                         const uint8_t *data, size_t len,
                         const vp_rpn_env_t *env, float *out)
{
    float st[VP_RPN_MAX_STACK];
    size_t sp = 0;
    size_t pc;

    if (!code || !out || (!data && len)) return VP_RPN_ERR_ARG;
    if (n == 0) return VP_RPN_ERR_EMPTY;
    if (n > VP_RPN_MAX_INSNS) return VP_RPN_ERR_TOO_LONG;

    for (pc = 0; pc < n; pc++) {
        const vp_rpn_insn_t *in = &code[pc];
        uint8_t pops, pushes;
        float a = 0.0f, b = 0.0f, c = 0.0f, r = 0.0f;
        uint32_t ua, ub;

        if (!vp_rpn_op_arity(in->op, &pops, &pushes)) return VP_RPN_ERR_OPCODE;
        if (sp < pops) return VP_RPN_ERR_UNDERFLOW;
        if (sp - pops + pushes > VP_RPN_MAX_STACK) return VP_RPN_ERR_OVERFLOW;

        /* operands in push order: a (deepest) .. c */
        if (pops == 3) {
            a = st[sp - 3]; b = st[sp - 2]; c = st[sp - 1];
        } else if (pops == 2) {
            a = st[sp - 2]; b = st[sp - 1];
        } else if (pops == 1) {
            a = st[sp - 1];
        }
        sp -= pops;

        switch (in->op) {
        case VP_OP_PUSH_BYTE:
            if (in->u.arg >= len) return VP_RPN_ERR_DATA;
            r = (float)data[in->u.arg];
            break;
        case VP_OP_PUSH_RAW: {
            int64_t v;
            if (in->bits == 0 || in->bits > VP_RPN_MAX_RAWBITS || in->u.arg > UINT16_MAX) {
                return VP_RPN_ERR_RAW;
            }
            if (!vp_rpn_extract_raw(data, len, (uint16_t)in->u.arg, in->bits, in->flags, &v)) {
                return VP_RPN_ERR_DATA;
            }
            /* Extraction returns at most a signed or unsigned 32-bit field. */
            r = (in->flags & VP_RAW_SIGNED) ? (float)(int32_t)v : (float)(uint32_t)v;
            break;
        }
        case VP_OP_PUSH_CONST:
            if (!const_ok(in->u.k)) return VP_RPN_ERR_CONST;
            r = in->u.k;
            break;
        case VP_OP_ADD: r = a + b; break;
        case VP_OP_SUB: r = a - b; break;
        case VP_OP_MUL: r = a * b; break;
        case VP_OP_DIV:
            if (b == 0.0f) return VP_RPN_ERR_DIV0;
            r = a / b;
            break;
        case VP_OP_NEG: r = -a; break;
        case VP_OP_MIN:
            if (isnan(a) || isnan(b)) return VP_RPN_ERR_NONFINITE;
            r = (a < b) ? a : b;
            break;
        case VP_OP_MAX:
            if (isnan(a) || isnan(b)) return VP_RPN_ERR_NONFINITE;
            r = (a > b) ? a : b;
            break;
        case VP_OP_CLAMP:
            if (isnan(a) || isnan(b) || isnan(c)) return VP_RPN_ERR_NONFINITE;
            if (b > c) return VP_RPN_ERR_CLAMP;
            r = (a < b) ? b : ((a > c) ? c : a);
            break;
        case VP_OP_AND:
        case VP_OP_OR:
            if (!to_u32(a, &ua) || !to_u32(b, &ub)) return VP_RPN_ERR_INT;
            r = (float)((in->op == VP_OP_AND) ? (ua & ub) : (ua | ub));
            break;
        case VP_OP_SHR:
        case VP_OP_SHL:
            if (!to_u32(a, &ua) || !to_shift(b, &ub)) return VP_RPN_ERR_INT;
            r = (float)((in->op == VP_OP_SHR) ? (ua >> ub) : (uint32_t)(ua << ub));
            break;
        case VP_OP_SEXT8:
            if (!to_u32(a, &ua)) return VP_RPN_ERR_INT;
            ua &= 0xFFu;
            r = (float)((ua & 0x80u) ? (int32_t)ua - 0x100 : (int32_t)ua);
            break;
        case VP_OP_SEXT16:
            if (!to_u32(a, &ua)) return VP_RPN_ERR_INT;
            ua &= 0xFFFFu;
            r = (float)((ua & 0x8000u) ? (int32_t)ua - 0x10000 : (int32_t)ua);
            break;
        case VP_OP_LUT: {
            vp_rpn_err_t e;
            if (isnan(a)) return VP_RPN_ERR_NONFINITE;
            e = lut_lookup(env, in->u.arg, a, &r);
            if (e != VP_RPN_OK) return e;
            break;
        }
        default:
            return VP_RPN_ERR_OPCODE;
        }
        if (!isfinite(r)) return VP_RPN_ERR_NONFINITE;
        st[sp++] = r;
    }
    if (sp != 1) return VP_RPN_ERR_RESULT;
    *out = st[0];
    return VP_RPN_OK;
}
