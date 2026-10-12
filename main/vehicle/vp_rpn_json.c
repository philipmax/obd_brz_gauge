#include "vp_rpn.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"

/* Bare-string opcodes (no operand). */
static const struct {
    const char *name;
    uint8_t     op;
} s_bare_ops[] = {
    { "+",      VP_OP_ADD },
    { "-",      VP_OP_SUB },
    { "*",      VP_OP_MUL },
    { "/",      VP_OP_DIV },
    { "neg",    VP_OP_NEG },
    { "min",    VP_OP_MIN },
    { "max",    VP_OP_MAX },
    { "clamp",  VP_OP_CLAMP },
    { "&",      VP_OP_AND },
    { "|",      VP_OP_OR },
    { ">>",     VP_OP_SHR },
    { "<<",     VP_OP_SHL },
    { "sext8",  VP_OP_SEXT8 },
    { "sext16", VP_OP_SEXT16 },
};

static bool json_uint(const cJSON *it, uint32_t max, uint32_t *out)
{
    double d;
    if (!cJSON_IsNumber(it)) return false;
    d = it->valuedouble;
    if (!isfinite(d) || d < 0.0 || d > (double)max || d != floor(d)) return false;
    *out = (uint32_t)d;
    return true;
}

static vp_rpn_err_t decode_raw(const cJSON *op, vp_rpn_insn_t *in)
{
    int n = cJSON_GetArraySize(op);
    uint32_t start, bits;
    bool have_endian = false, have_sign = false;
    int i;

    if (n < 3 || n > 5) return VP_RPN_ERR_OPCODE;
    if (!json_uint(cJSON_GetArrayItem(op, 1), UINT16_MAX, &start)) return VP_RPN_ERR_RAW;
    if (!json_uint(cJSON_GetArrayItem(op, 2), 255, &bits)) return VP_RPN_ERR_RAW;
    if (bits == 0 || bits > VP_RPN_MAX_RAWBITS) return VP_RPN_ERR_RAW;
    in->op = VP_OP_PUSH_RAW;
    in->u.arg = start;
    in->bits = (uint8_t)bits;
    in->flags = 0;
    for (i = 3; i < n; i++) {
        const char *s = cJSON_GetStringValue(cJSON_GetArrayItem(op, i));
        if (!s) return VP_RPN_ERR_OPCODE;
        if (!strcmp(s, "le") || !strcmp(s, "be")) {
            if (have_endian) return VP_RPN_ERR_OPCODE;
            have_endian = true;
            if (s[0] == 'b') in->flags |= VP_RAW_BE;
        } else if (!strcmp(s, "u") || !strcmp(s, "s")) {
            if (have_sign) return VP_RPN_ERR_OPCODE;
            have_sign = true;
            if (s[0] == 's') in->flags |= VP_RAW_SIGNED;
        } else {
            return VP_RPN_ERR_OPCODE;
        }
    }
    return VP_RPN_OK;
}

static vp_rpn_err_t decode_one(const cJSON *op, const vp_rpn_raw_t *x_field, vp_rpn_insn_t *in)
{
    const char *tag;
    int n;
    size_t i;

    memset(in, 0, sizeof(*in));
    if (cJSON_IsString(op)) {
        for (i = 0; i < sizeof(s_bare_ops) / sizeof(s_bare_ops[0]); i++) {
            if (strcmp(op->valuestring, s_bare_ops[i].name) == 0) {
                in->op = s_bare_ops[i].op;
                return VP_RPN_OK;
            }
        }
        return VP_RPN_ERR_OPCODE;
    }
    if (!cJSON_IsArray(op)) return VP_RPN_ERR_OPCODE;
    n = cJSON_GetArraySize(op);
    tag = cJSON_GetStringValue(cJSON_GetArrayItem(op, 0));
    if (!tag) return VP_RPN_ERR_OPCODE;

    if (!strcmp(tag, "A")) {
        uint32_t idx;
        if (n != 2) return VP_RPN_ERR_OPCODE;
        if (!json_uint(cJSON_GetArrayItem(op, 1), 255, &idx)) return VP_RPN_ERR_INDEX;
        in->op = VP_OP_PUSH_BYTE;
        in->u.arg = idx;
        return VP_RPN_OK;
    }
    if (!strcmp(tag, "k")) {
        const cJSON *v = cJSON_GetArrayItem(op, 1);
        double d;
        if (n != 2 || !cJSON_IsNumber(v)) return VP_RPN_ERR_OPCODE;
        d = v->valuedouble;
        if (!isfinite(d) || fabs(d) > VP_RPN_MAX_CONST) return VP_RPN_ERR_CONST;
        in->op = VP_OP_PUSH_CONST;
        in->u.k = (float)d;
        return VP_RPN_OK;
    }
    if (!strcmp(tag, "R")) {
        return decode_raw(op, in);
    }
    if (!strcmp(tag, "X")) {
        if (n != 1) return VP_RPN_ERR_OPCODE;
        if (!x_field) return VP_RPN_ERR_X;
        in->op = VP_OP_PUSH_RAW;
        in->u.arg = x_field->start;
        in->bits = x_field->bits;
        in->flags = x_field->flags;
        return VP_RPN_OK;
    }
    if (!strcmp(tag, "lut")) {
        uint32_t idx;
        if (n != 2) return VP_RPN_ERR_OPCODE;
        if (!json_uint(cJSON_GetArrayItem(op, 1), 255, &idx)) return VP_RPN_ERR_LUT;
        in->op = VP_OP_LUT;
        in->u.arg = idx;
        return VP_RPN_OK;
    }
    return VP_RPN_ERR_OPCODE;
}

vp_rpn_err_t vp_rpn_from_json(const struct cJSON *arr, const vp_rpn_raw_t *x_field,
                              vp_rpn_insn_t *out, size_t cap, size_t *n_out,
                              size_t *err_index)
{
    const cJSON *op;
    size_t n = 0;

    if (n_out) *n_out = 0;
    if (err_index) *err_index = 0;
    if (!arr || !out || !cJSON_IsArray(arr)) return VP_RPN_ERR_OPCODE;

    cJSON_ArrayForEach(op, arr) {
        vp_rpn_err_t e;
        if (n >= cap || n >= VP_RPN_MAX_INSNS) {
            if (err_index) *err_index = n;
            return VP_RPN_ERR_TOO_LONG;
        }
        e = decode_one(op, x_field, &out[n]);
        if (e != VP_RPN_OK) {
            if (err_index) *err_index = n;
            return e;
        }
        n++;
    }
    if (n == 0) return VP_RPN_ERR_EMPTY;
    if (n_out) *n_out = n;
    return VP_RPN_OK;
}
