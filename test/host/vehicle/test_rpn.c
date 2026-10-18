/* RPN executor, verifier and JSON decoder tests. */
#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "cJSON.h"
#include "vp_rpn.h"
#include "vt.h"

static vp_rpn_insn_t I_B(uint32_t i)
{
    vp_rpn_insn_t in = { VP_OP_PUSH_BYTE, 0, 0, 0, { 0 } };
    in.u.arg = i;
    return in;
}

static vp_rpn_insn_t I_K(float k)
{
    vp_rpn_insn_t in = { VP_OP_PUSH_CONST, 0, 0, 0, { 0 } };
    in.u.k = k;
    return in;
}

static vp_rpn_insn_t I_R(uint32_t start, uint8_t bits, uint8_t flags)
{
    vp_rpn_insn_t in = { VP_OP_PUSH_RAW, 0, 0, 0, { 0 } };
    in.u.arg = start;
    in.bits = bits;
    in.flags = flags;
    return in;
}

static vp_rpn_insn_t I_L(uint32_t idx)
{
    vp_rpn_insn_t in = { VP_OP_LUT, 0, 0, 0, { 0 } };
    in.u.arg = idx;
    return in;
}

static vp_rpn_insn_t I_OP(uint8_t op)
{
    vp_rpn_insn_t in = { 0, 0, 0, 0, { 0 } };
    in.op = op;
    return in;
}

#define N(a) (sizeof(a) / sizeof((a)[0]))

static vp_rpn_err_t eval(const vp_rpn_insn_t *c, size_t n, const uint8_t *d, size_t len, float *out)
{
    return vp_rpn_eval(c, n, d, len, NULL, out);
}

static vp_rpn_err_t verify(const vp_rpn_insn_t *c, size_t n, uint16_t len, uint16_t luts, size_t *pc)
{
    vp_rpn_verify_cfg_t vc;
    vc.data_len = len;
    vc.n_luts = luts;
    return vp_rpn_verify(c, n, &vc, pc);
}

/* binary op on two constants */
static vp_rpn_err_t bin(uint8_t op, float a, float b, float *out)
{
    vp_rpn_insn_t c[3];
    c[0] = I_K(a);
    c[1] = I_K(b);
    c[2] = I_OP(op);
    return eval(c, 3, NULL, 0, out);
}

static vp_rpn_err_t un(uint8_t op, float a, float *out)
{
    vp_rpn_insn_t c[2];
    c[0] = I_K(a);
    c[1] = I_OP(op);
    return eval(c, 2, NULL, 0, out);
}

static void test_push_byte(void)
{
    const uint8_t d[2] = { 0x12, 0x34 };
    vp_rpn_insn_t c[1];
    float r = 0;
    size_t pc = 99;

    c[0] = I_B(0);
    VT_CHECK_INT(eval(c, 1, d, 2, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0x12, 0);
    c[0] = I_B(1);
    VT_CHECK_INT(eval(c, 1, d, 2, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0x34, 0);
    c[0] = I_B(2);
    r = -1;
    VT_CHECK_INT(eval(c, 1, d, 2, &r), VP_RPN_ERR_DATA);
    VT_CHECK_FLT(r, -1, 0);   /* untouched on error */
    VT_CHECK_INT(verify(c, 1, 2, 0, &pc), VP_RPN_ERR_INDEX);
    VT_CHECK_INT(pc, 0);
    VT_CHECK_INT(verify(c, 1, 3, 0, &pc), VP_RPN_OK);
}

static void test_push_raw(void)
{
    const uint8_t d[4] = { 0x34, 0x12, 0xFF, 0x80 };
    vp_rpn_insn_t c[1];
    float r = 0;
    int64_t v = 0;

    c[0] = I_R(0, 16, 0);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0x1234, 0);
    c[0] = I_R(0, 16, VP_RAW_BE);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0x3412, 0);
    c[0] = I_R(16, 8, VP_RAW_SIGNED);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -1, 0);
    c[0] = I_R(24, 8, VP_RAW_SIGNED);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -128, 0);
    c[0] = I_R(4, 8, 0);   /* LE: d0 bits 4..7 then d1 bits 0..3 */
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0x23, 0);
    c[0] = I_R(4, 8, VP_RAW_BE);   /* BE: low nibble of d0, high nibble of d1 */
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0x41, 0);
    c[0] = I_R(16, 16, VP_RAW_BE | VP_RAW_SIGNED);   /* 0xFF80 */
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -128, 0);
    c[0] = I_R(0, 32, 0);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 2164199988.0, 256);
    c[0] = I_R(0, 32, VP_RAW_SIGNED);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, (double)(int32_t)0x80FF1234u, 256);
    c[0] = I_R(1, 1, 0);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0, 0);
    c[0] = I_R(2, 1, 0);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 1, 0);

    /* bounds */
    c[0] = I_R(24, 16, 0);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_ERR_DATA);
    VT_CHECK_INT(verify(c, 1, 4, 0, NULL), VP_RPN_ERR_RAW);
    VT_CHECK_INT(verify(c, 1, 5, 0, NULL), VP_RPN_OK);
    c[0] = I_R(0, 0, 0);
    VT_CHECK_INT(verify(c, 1, 4, 0, NULL), VP_RPN_ERR_RAW);
    VT_CHECK_INT(eval(c, 1, d, 4, &r), VP_RPN_ERR_RAW);
    c[0] = I_R(0, 33, 0);
    VT_CHECK_INT(verify(c, 1, 8, 0, NULL), VP_RPN_ERR_RAW);
    c[0] = I_R(0, 8, 0x80);
    VT_CHECK_INT(verify(c, 1, 8, 0, NULL), VP_RPN_ERR_RAW);

    /* ZC6 CAN 0x360: oil temp in byte 2 (bit 16, 8 bits, LE) */
    {
        const uint8_t f[8] = { 0, 0, 0x82, 0x7A, 0, 0, 0, 0 };
        VT_CHECK(vp_rpn_extract_raw(f, 8, 16, 8, 0, &v));
        VT_CHECK_INT(v, 0x82);
        VT_CHECK(vp_rpn_extract_raw(f, 8, 24, 8, 0, &v));
        VT_CHECK_INT(v, 0x7A);
        VT_CHECK(!vp_rpn_extract_raw(f, 8, 60, 8, 0, &v));
        VT_CHECK(!vp_rpn_extract_raw(NULL, 8, 0, 8, 0, &v));
    }
}

/* Independent bit-by-bit oracle: exercise every bit offset/width and
 * both endiannesses, including signed 32-bit fields and truncated buffers. */
static bool raw_reference(const uint8_t *data, size_t len, uint16_t start,
                          uint8_t bits, uint8_t flags, int64_t *out)
{
    uint32_t v = 0;
    if (!data || !out || !bits || bits > 32) return false;
    if ((uint32_t)start + bits > len * 8u) return false;
    for (unsigned i = 0; i < bits; i++) {
        unsigned pos = start + i;
        if (flags & VP_RAW_BE)
            v = (v << 1) | ((data[pos / 8] >> (7u - pos % 8)) & 1u);
        else
            v |= (uint32_t)((data[pos / 8] >> (pos % 8)) & 1u) << i;
    }
    *out = ((flags & VP_RAW_SIGNED) && (v & (1u << (bits - 1))))
         ? (int64_t)v - ((int64_t)1 << bits) : (int64_t)v;
    return true;
}

static void test_raw_all_fields(void)
{
    uint8_t storage[10];
    uint8_t *data = storage + 1;
    uint32_t seed = 0x617E9AB3u;
    for (unsigned pattern = 0; pattern < 12; pattern++) {
        for (unsigned i = 0; i < 8; i++) {
            seed = seed * 1664525u + 1013904223u;
            data[i] = pattern == 0 ? 0 : pattern == 1 ? 0xFF :
                      pattern == 2 ? 0xAA : pattern == 3 ? 0x55 : (uint8_t)(seed >> 24);
        }
        for (uint16_t start = 0; start <= 64; start++) {
            for (uint8_t bits = 0; bits <= 33; bits++) {
                for (uint8_t flags = 0; flags < 4; flags++) {
                    size_t needed = ((size_t)start + bits + 7) / 8;
                    /* Exact end boundary, one byte short, and a full frame. */
                    size_t lengths[] = {needed <= 8 ? needed : 8, needed ? needed - 1 : 0, 8};
                    for (unsigned k = 0; k < N(lengths); k++) {
                        if (lengths[k] > 8) continue;
                        int64_t expected = INT64_C(0x123456789), actual = expected;
                        bool ok = raw_reference(data, lengths[k], start, bits, flags, &expected);
                        VT_CHECK_INT(vp_rpn_extract_raw(data, lengths[k], start, bits, flags, &actual), ok);
                        VT_CHECK_INT(actual, expected); /* errors must leave output untouched */
                    }
                }
            }
        }
    }
    int64_t out = 123;
    VT_CHECK(!vp_rpn_extract_raw(NULL, 8, 0, 8, 0, &out));
    VT_CHECK(!vp_rpn_extract_raw(data, 8, 0, 8, 0, NULL));
    VT_CHECK_INT(out, 123);
    uint8_t far_data[8196] = {0};
    far_data[8191] = 0xD3;
    far_data[8192] = 0x79;
    far_data[8193] = 0xA6;
    far_data[8194] = 0xCC;
    far_data[8195] = 0x01;
    for (uint8_t flags = 0; flags < 4; flags++) {
        int64_t expected;
        VT_CHECK(raw_reference(far_data, sizeof(far_data), UINT16_MAX, 32, flags, &expected));
        VT_CHECK(vp_rpn_extract_raw(far_data, sizeof(far_data), UINT16_MAX, 32, flags, &out));
        VT_CHECK_INT(out, expected);
    }
}

static void test_const(void)
{
    vp_rpn_insn_t c[1];
    float r = 0;

    c[0] = I_K(1.5f);
    VT_CHECK_INT(eval(c, 1, NULL, 0, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 1.5, 0);
    c[0] = I_K(-1e7f);
    VT_CHECK_INT(verify(c, 1, 0, 0, NULL), VP_RPN_OK);
    c[0] = I_K(NAN);
    VT_CHECK_INT(verify(c, 1, 0, 0, NULL), VP_RPN_ERR_CONST);
    VT_CHECK_INT(eval(c, 1, NULL, 0, &r), VP_RPN_ERR_CONST);
    c[0] = I_K(INFINITY);
    VT_CHECK_INT(verify(c, 1, 0, 0, NULL), VP_RPN_ERR_CONST);
    c[0] = I_K(1.0001e7f);
    VT_CHECK_INT(verify(c, 1, 0, 0, NULL), VP_RPN_ERR_CONST);
}

static void test_arith(void)
{
    float r = 0;
    VT_CHECK_INT(bin(VP_OP_ADD, 5, 3, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 8, 0);
    VT_CHECK_INT(bin(VP_OP_SUB, 5, 3, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 2, 0);   /* a b - == a-b */
    VT_CHECK_INT(bin(VP_OP_MUL, 5, 3, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 15, 0);
    VT_CHECK_INT(bin(VP_OP_DIV, 7, 2, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 3.5, 0);
    VT_CHECK_INT(un(VP_OP_NEG, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -4, 0);
    VT_CHECK_INT(bin(VP_OP_MIN, 5, 3, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 3, 0);
    VT_CHECK_INT(bin(VP_OP_MAX, 5, 3, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 5, 0);
    VT_CHECK_INT(bin(VP_OP_MAX, -2, 0, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0, 0);
}

static void test_clamp(void)
{
    vp_rpn_insn_t c[4];
    float r = 0;
    const float cases[][4] = {   /* x lo hi expected */
        { 5, 0, 3, 3 }, { -1, 0, 3, 0 }, { 2, 0, 3, 2 }, { 3, 3, 3, 3 },
    };
    size_t i;

    for (i = 0; i < N(cases); i++) {
        c[0] = I_K(cases[i][0]);
        c[1] = I_K(cases[i][1]);
        c[2] = I_K(cases[i][2]);
        c[3] = I_OP(VP_OP_CLAMP);
        VT_CHECK_INT(eval(c, 4, NULL, 0, &r), VP_RPN_OK);
        VT_CHECK_FLT(r, cases[i][3], 0);
    }
    c[0] = I_K(1);
    c[1] = I_K(4);
    c[2] = I_K(3);
    VT_CHECK_INT(eval(c, 4, NULL, 0, &r), VP_RPN_ERR_CLAMP);
    VT_CHECK_INT(verify(c, 3, 0, 0, NULL), VP_RPN_ERR_RESULT);
    VT_CHECK_INT(verify(c, 4, 0, 0, NULL), VP_RPN_OK);
}

static void test_div0(void)
{
    const uint8_t d[2] = { 1, 0 };
    vp_rpn_insn_t c[3];
    float r = 0;
    size_t pc = 0;

    c[0] = I_B(0);
    c[1] = I_B(1);
    c[2] = I_OP(VP_OP_DIV);
    VT_CHECK_INT(verify(c, 3, 2, 0, NULL), VP_RPN_OK);
    VT_CHECK_INT(eval(c, 3, d, 2, &r), VP_RPN_ERR_DIV0);
    c[1] = I_K(0);
    VT_CHECK_INT(verify(c, 3, 2, 0, &pc), VP_RPN_ERR_DIV0);
    VT_CHECK_INT(pc, 2);
    c[1] = I_K(-0.0f);
    VT_CHECK_INT(verify(c, 3, 2, 0, NULL), VP_RPN_ERR_DIV0);
    VT_CHECK_INT(eval(c, 3, d, 2, &r), VP_RPN_ERR_DIV0);
    /* constant zero as dividend is fine */
    c[0] = I_K(0);
    c[1] = I_B(0);
    VT_CHECK_INT(verify(c, 3, 2, 0, NULL), VP_RPN_OK);
    VT_CHECK_INT(eval(c, 3, d, 2, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0, 0);
}

static void test_int_ops(void)
{
    float r = 0;
    VT_CHECK_INT(bin(VP_OP_AND, 0xF3, 0x0F, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 3, 0);
    VT_CHECK_INT(bin(VP_OP_OR, 0xF0, 0x0F, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0xFF, 0);
    VT_CHECK_INT(bin(VP_OP_SHR, 0xF0, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0x0F, 0);
    VT_CHECK_INT(bin(VP_OP_SHL, 3, 4, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 48, 0);
    VT_CHECK_INT(bin(VP_OP_AND, -1, 0xFF, &r), VP_RPN_OK);   /* two's complement pattern */
    VT_CHECK_FLT(r, 255, 0);
    VT_CHECK_INT(bin(VP_OP_AND, 7.9f, 3, &r), VP_RPN_OK);    /* truncation toward zero */
    VT_CHECK_FLT(r, 3, 0);
    VT_CHECK_INT(bin(VP_OP_SHR, -2, 31, &r), VP_RPN_OK);     /* logical shift */
    VT_CHECK_FLT(r, 1, 0);
    {
        /* 2^31 << 1 drops the bit: build 2^31 as 65536*32768 */
        vp_rpn_insn_t c[5];
        c[0] = I_K(65536);
        c[1] = I_K(32768);
        c[2] = I_OP(VP_OP_MUL);
        c[3] = I_K(1);
        c[4] = I_OP(VP_OP_SHL);
        VT_CHECK_INT(eval(c, 5, NULL, 0, &r), VP_RPN_OK);
        VT_CHECK_FLT(r, 0, 0);
        c[3] = I_K(0);
        VT_CHECK_INT(eval(c, 5, NULL, 0, &r), VP_RPN_OK);
        VT_CHECK_FLT(r, 2147483648.0, 0);
    }
    /* range errors */
    VT_CHECK_INT(bin(VP_OP_SHL, 1, 32, &r), VP_RPN_ERR_INT);
    VT_CHECK_INT(bin(VP_OP_SHR, 1, -1, &r), VP_RPN_ERR_INT);
    {
        vp_rpn_insn_t c[5];   /* 5e9 & 1 -> out of range */
        c[0] = I_K(5e6f);
        c[1] = I_K(1000);
        c[2] = I_OP(VP_OP_MUL);
        c[3] = I_K(1);
        c[4] = I_OP(VP_OP_AND);
        VT_CHECK_INT(eval(c, 5, NULL, 0, &r), VP_RPN_ERR_INT);
        c[0] = I_K(-3e6f);    /* -3e9 */
        VT_CHECK_INT(eval(c, 5, NULL, 0, &r), VP_RPN_ERR_INT);
        c[0] = I_K(-2e6f);    /* -2e9 fits */
        VT_CHECK_INT(eval(c, 5, NULL, 0, &r), VP_RPN_OK);
    }
}

static void test_sext(void)
{
    float r = 0;
    VT_CHECK_INT(un(VP_OP_SEXT8, 0xFF, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -1, 0);
    VT_CHECK_INT(un(VP_OP_SEXT8, 0x7F, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 127, 0);
    VT_CHECK_INT(un(VP_OP_SEXT8, 0x180, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -128, 0);
    VT_CHECK_INT(un(VP_OP_SEXT16, 0xFFFE, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -2, 0);
    VT_CHECK_INT(un(VP_OP_SEXT16, 0x8000, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -32768, 0);
    VT_CHECK_INT(un(VP_OP_SEXT16, 0x7FFF, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 32767, 0);
    VT_CHECK_INT(un(VP_OP_SEXT16, -1e7f, &r), VP_RPN_OK);
    {
        /* word from two bytes, then sign-extend: 0xFF 0x38 -> -200 */
        const uint8_t d[2] = { 0xFF, 0x38 };
        vp_rpn_insn_t c[6];
        c[0] = I_B(0);
        c[1] = I_K(256);
        c[2] = I_OP(VP_OP_MUL);
        c[3] = I_B(1);
        c[4] = I_OP(VP_OP_ADD);
        c[5] = I_OP(VP_OP_SEXT16);
        VT_CHECK_INT(eval(c, 6, d, 2, &r), VP_RPN_OK);
        VT_CHECK_FLT(r, -200, 0);
    }
}

static void test_lut(void)
{
    const vp_rpn_lut_entry_t e[] = {
        { 0, 0, 0 }, { 1, 6, 1 }, { 7, 7, -1 },   /* table 0: N, D(1..6), R */
        { 10, 20, 100 },                          /* table 1 */
    };
    const vp_rpn_lut_t t[] = { { 0, 3, 0 }, { 3, 1, 0 } };
    vp_rpn_env_t env = { t, e, 2, 4 };
    vp_rpn_insn_t c[2];
    float r = 0;
    uint8_t d[1];

    c[0] = I_B(0);
    c[1] = I_L(0);
    d[0] = 0;
    VT_CHECK_INT(vp_rpn_eval(c, 2, d, 1, &env, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0, 0);
    d[0] = 4;
    VT_CHECK_INT(vp_rpn_eval(c, 2, d, 1, &env, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 1, 0);
    d[0] = 7;
    VT_CHECK_INT(vp_rpn_eval(c, 2, d, 1, &env, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -1, 0);
    d[0] = 8;
    VT_CHECK_INT(vp_rpn_eval(c, 2, d, 1, &env, &r), VP_RPN_ERR_LUT_MISS);
    c[1] = I_L(1);
    d[0] = 15;
    VT_CHECK_INT(vp_rpn_eval(c, 2, d, 1, &env, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 100, 0);
    c[1] = I_L(2);
    VT_CHECK_INT(vp_rpn_eval(c, 2, d, 1, &env, &r), VP_RPN_ERR_LUT);
    VT_CHECK_INT(verify(c, 2, 1, 2, NULL), VP_RPN_ERR_LUT);
    VT_CHECK_INT(verify(c, 2, 1, 3, NULL), VP_RPN_OK);
    c[1] = I_L(0);
    VT_CHECK_INT(vp_rpn_eval(c, 2, d, 1, NULL, &r), VP_RPN_ERR_LUT);
    {
        /* corrupt table bounds are caught at runtime */
        const vp_rpn_lut_t bad[] = { { 2, 5, 0 } };
        vp_rpn_env_t benv = { bad, e, 1, 4 };
        VT_CHECK_INT(vp_rpn_eval(c, 2, d, 1, &benv, &r), VP_RPN_ERR_LUT);
    }
}

static void test_stack(void)
{
    vp_rpn_insn_t c[VP_RPN_MAX_INSNS + 1];
    float r = 0;
    size_t pc = 0, i;

    c[0] = I_OP(VP_OP_ADD);
    VT_CHECK_INT(verify(c, 1, 0, 0, &pc), VP_RPN_ERR_UNDERFLOW);
    VT_CHECK_INT(pc, 0);
    VT_CHECK_INT(eval(c, 1, NULL, 0, &r), VP_RPN_ERR_UNDERFLOW);
    c[0] = I_K(1);
    c[1] = I_OP(VP_OP_ADD);
    VT_CHECK_INT(verify(c, 2, 0, 0, &pc), VP_RPN_ERR_UNDERFLOW);
    VT_CHECK_INT(pc, 1);
    c[0] = I_K(1);
    c[1] = I_K(2);
    c[2] = I_OP(VP_OP_CLAMP);
    VT_CHECK_INT(verify(c, 3, 0, 0, &pc), VP_RPN_ERR_UNDERFLOW);

    /* depth 8 is allowed, 9 is not */
    for (i = 0; i < 8; i++) c[i] = I_K((float)i);
    for (i = 8; i < 15; i++) c[i] = I_OP(VP_OP_ADD);
    VT_CHECK_INT(verify(c, 15, 0, 0, NULL), VP_RPN_OK);
    VT_CHECK_INT(eval(c, 15, NULL, 0, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 28, 0);
    for (i = 0; i < 9; i++) c[i] = I_K((float)i);
    for (i = 9; i < 17; i++) c[i] = I_OP(VP_OP_ADD);
    VT_CHECK_INT(verify(c, 17, 0, 0, &pc), VP_RPN_ERR_OVERFLOW);
    VT_CHECK_INT(pc, 8);
    VT_CHECK_INT(eval(c, 17, NULL, 0, &r), VP_RPN_ERR_OVERFLOW);

    /* result count */
    c[0] = I_K(1);
    c[1] = I_K(2);
    VT_CHECK_INT(verify(c, 2, 0, 0, &pc), VP_RPN_ERR_RESULT);
    VT_CHECK_INT(pc, 2);
    VT_CHECK_INT(eval(c, 2, NULL, 0, &r), VP_RPN_ERR_RESULT);
    VT_CHECK_INT(verify(c, 0, 0, 0, NULL), VP_RPN_ERR_EMPTY);
    VT_CHECK_INT(eval(c, 0, NULL, 0, &r), VP_RPN_ERR_EMPTY);

    /* length: 32 ok, 33 rejected */
    c[0] = I_K(1);
    for (i = 1; i + 1 < VP_RPN_MAX_INSNS; i += 2) {
        c[i] = I_K(1);
        c[i + 1] = I_OP(VP_OP_ADD);
    }
    c[VP_RPN_MAX_INSNS - 1] = I_OP(VP_OP_NEG);
    VT_CHECK_INT(verify(c, VP_RPN_MAX_INSNS, 0, 0, NULL), VP_RPN_OK);
    VT_CHECK_INT(eval(c, VP_RPN_MAX_INSNS, NULL, 0, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, -16, 0);
    c[VP_RPN_MAX_INSNS] = I_OP(VP_OP_NEG);
    VT_CHECK_INT(verify(c, VP_RPN_MAX_INSNS + 1, 0, 0, NULL), VP_RPN_ERR_TOO_LONG);
    VT_CHECK_INT(eval(c, VP_RPN_MAX_INSNS + 1, NULL, 0, &r), VP_RPN_ERR_TOO_LONG);
}

static void test_nonfinite(void)
{
    vp_rpn_insn_t c[11];
    float r = 0;
    size_t i;

    /* 1e7^6 overflows float32 */
    c[0] = I_K(1e7f);
    for (i = 1; i < 11; i += 2) {
        c[i] = I_K(1e7f);
        c[i + 1] = I_OP(VP_OP_MUL);
    }
    VT_CHECK_INT(verify(c, 11, 0, 0, NULL), VP_RPN_OK);
    VT_CHECK_INT(eval(c, 11, NULL, 0, &r), VP_RPN_ERR_NONFINITE);
    VT_CHECK_INT(eval(c, 7, NULL, 0, &r), VP_RPN_OK);   /* 1e28 still finite */
    VT_CHECK_FLT(r / 1e28, 1.0, 1e-6);
    /* tiny divisor -> overflow */
    VT_CHECK_INT(bin(VP_OP_DIV, 1e7f, 1e-38f, &r), VP_RPN_ERR_NONFINITE);
}

static void test_opcode(void)
{
    vp_rpn_insn_t c[1];
    float r = 0;
    uint8_t pops, pushes, op;

    c[0] = I_OP(0);
    VT_CHECK_INT(verify(c, 1, 0, 0, NULL), VP_RPN_ERR_OPCODE);
    VT_CHECK_INT(eval(c, 1, NULL, 0, &r), VP_RPN_ERR_OPCODE);
    c[0] = I_OP(VP_OP_COUNT);
    VT_CHECK_INT(verify(c, 1, 0, 0, NULL), VP_RPN_ERR_OPCODE);
    c[0] = I_OP(0xEE);
    VT_CHECK_INT(eval(c, 1, NULL, 0, &r), VP_RPN_ERR_OPCODE);
    for (op = 1; op < VP_OP_COUNT; op++) {
        VT_CHECK(vp_rpn_op_arity(op, &pops, &pushes));
        VT_CHECK_INT(pushes, 1);
        VT_CHECK(vp_rpn_op_name(op) != NULL);
    }
    VT_CHECK(!vp_rpn_op_arity(0, &pops, &pushes));
    VT_CHECK(vp_rpn_op_name(VP_OP_COUNT) == NULL);
    VT_CHECK_INT(eval(NULL, 1, NULL, 0, &r), VP_RPN_ERR_ARG);
    VT_CHECK_INT(verify(c, 1, 0, 0, NULL) != VP_RPN_OK, 1);
    VT_CHECK(vp_rpn_err_str(VP_RPN_ERR_DIV0)[0] != '?');
}

/* ---- JSON decoding ---- */

static vp_rpn_err_t dec(const char *json, const vp_rpn_raw_t *x, vp_rpn_insn_t *out, size_t *n, size_t *at)
{
    cJSON *a = cJSON_Parse(json);
    vp_rpn_err_t e;
    if (!a) return (vp_rpn_err_t)-1;
    e = vp_rpn_from_json(a, x, out, VP_RPN_MAX_INSNS, n, at);
    cJSON_Delete(a);
    return e;
}

static void test_json_example(void)
{
    vp_rpn_insn_t c[VP_RPN_MAX_INSNS];
    size_t n = 0;
    float r = 0;
    const uint8_t d[2] = { 0x1A, 0xF8 };

    VT_CHECK_INT(dec("[[\"A\",0],[\"k\",256],\"*\",[\"A\",1],\"+\",[\"k\",4],\"/\"]", NULL, c, &n, NULL),
                 VP_RPN_OK);
    VT_CHECK_INT(n, 7);
    VT_CHECK_INT(verify(c, n, 2, 0, NULL), VP_RPN_OK);
    VT_CHECK_INT(eval(c, n, d, 2, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 1726, 0);   /* 0x1AF8 = 6904 */
    VT_CHECK_INT(verify(c, n, 1, 0, NULL), VP_RPN_ERR_INDEX);
}

static void test_json_opcodes(void)
{
    /* every opcode's canonical spelling decodes to that opcode */
    static const char *const spell[VP_OP_COUNT] = {
        NULL, "[[\"A\",3]]", "[[\"R\",8,12,\"be\",\"s\"]]", "[[\"k\",-2.5]]", "[\"+\"]", "[\"-\"]",
        "[\"*\"]", "[\"/\"]", "[\"neg\"]", "[\"min\"]", "[\"max\"]", "[\"clamp\"]", "[\"&\"]",
        "[\"|\"]", "[\">>\"]", "[\"<<\"]", "[\"sext8\"]", "[\"sext16\"]", "[[\"lut\",2]]",
    };
    vp_rpn_insn_t c[VP_RPN_MAX_INSNS];
    size_t n = 0;
    uint8_t op;

    for (op = 1; op < VP_OP_COUNT; op++) {
        VT_CHECK_MSG(dec(spell[op], NULL, c, &n, NULL) == VP_RPN_OK, "%s", spell[op]);
        VT_CHECK_INT(n, 1);
        VT_CHECK_INT(c[0].op, op);
    }
    dec(spell[VP_OP_PUSH_RAW], NULL, c, &n, NULL);
    VT_CHECK_INT(c[0].u.arg, 8);
    VT_CHECK_INT(c[0].bits, 12);
    VT_CHECK_INT(c[0].flags, VP_RAW_BE | VP_RAW_SIGNED);
    dec("[[\"R\",0,8]]", NULL, c, &n, NULL);
    VT_CHECK_INT(c[0].flags, 0);
    dec("[[\"R\",0,8,\"s\",\"le\"]]", NULL, c, &n, NULL);
    VT_CHECK_INT(c[0].flags, VP_RAW_SIGNED);
    dec(spell[VP_OP_PUSH_CONST], NULL, c, &n, NULL);
    VT_CHECK_FLT(c[0].u.k, -2.5, 0);
    dec(spell[VP_OP_LUT], NULL, c, &n, NULL);
    VT_CHECK_INT(c[0].u.arg, 2);
    /* names table agrees with the decoder */
    VT_CHECK(!strcmp(vp_rpn_op_name(VP_OP_SHR), ">>"));
    VT_CHECK(!strcmp(vp_rpn_op_name(VP_OP_PUSH_BYTE), "A"));
}

static void test_json_x(void)
{
    vp_rpn_insn_t c[VP_RPN_MAX_INSNS];
    size_t n = 0, at = 0;
    vp_rpn_raw_t x = { 24, 8, 0 };
    float r = 0;
    const uint8_t frame[8] = { 0, 0, 0x82, 0x7A, 0, 0, 0, 0 };

    VT_CHECK_INT(dec("[[\"X\"],[\"k\",40],\"-\"]", NULL, c, &n, &at), VP_RPN_ERR_X);
    VT_CHECK_INT(at, 0);
    VT_CHECK_INT(dec("[[\"X\"],[\"k\",40],\"-\"]", &x, c, &n, NULL), VP_RPN_OK);
    VT_CHECK_INT(c[0].op, VP_OP_PUSH_RAW);
    VT_CHECK_INT(c[0].u.arg, 24);
    VT_CHECK_INT(c[0].bits, 8);
    VT_CHECK_INT(eval(c, n, frame, 8, &r), VP_RPN_OK);
    VT_CHECK_FLT(r, 0x7A - 40, 0);
}

static void test_json_errors(void)
{
    vp_rpn_insn_t c[VP_RPN_MAX_INSNS];
    size_t n = 0, at = 0;
    static const struct {
        const char  *json;
        vp_rpn_err_t err;
        size_t       at;
    } cases[] = {
        { "[]",                          VP_RPN_ERR_EMPTY,   0 },
        { "{\"op\":1}",                  VP_RPN_ERR_OPCODE,  0 },
        { "[\"pow\"]",                   VP_RPN_ERR_OPCODE,  0 },
        { "[[\"k\",1],\"^\"]",           VP_RPN_ERR_OPCODE,  1 },
        { "[[\"A\"]]",                   VP_RPN_ERR_OPCODE,  0 },
        { "[[\"A\",1,2]]",               VP_RPN_ERR_OPCODE,  0 },
        { "[[\"A\",-1]]",                VP_RPN_ERR_INDEX,   0 },
        { "[[\"A\",1.5]]",               VP_RPN_ERR_INDEX,   0 },
        { "[[\"A\",\"1\"]]",             VP_RPN_ERR_INDEX,   0 },
        { "[[\"A\",256]]",               VP_RPN_ERR_INDEX,   0 },
        { "[[\"k\",1e300]]",             VP_RPN_ERR_CONST,   0 },
        { "[[\"k\",10000001]]",          VP_RPN_ERR_CONST,   0 },
        { "[[\"k\",\"x\"]]",             VP_RPN_ERR_OPCODE,  0 },
        { "[[\"R\",0]]",                 VP_RPN_ERR_OPCODE,  0 },
        { "[[\"R\",0,0]]",               VP_RPN_ERR_RAW,     0 },
        { "[[\"R\",0,33]]",              VP_RPN_ERR_RAW,     0 },
        { "[[\"R\",0,8,\"xx\"]]",        VP_RPN_ERR_OPCODE,  0 },
        { "[[\"R\",0,8,\"le\",\"be\"]]", VP_RPN_ERR_OPCODE,  0 },
        { "[[\"R\",0,8,\"u\",\"s\"]]",   VP_RPN_ERR_OPCODE,  0 },
        { "[[\"lut\",-1]]",              VP_RPN_ERR_LUT,     0 },
        { "[[\"X\",1]]",                 VP_RPN_ERR_OPCODE,  0 },
        { "[[1,2]]",                     VP_RPN_ERR_OPCODE,  0 },
        { "[null]",                      VP_RPN_ERR_OPCODE,  0 },
        { "[[\"k\",1],[\"k\",2],[\"k\",3],[\"k\",4],[\"k\",5],[\"k\",6],[\"k\",7],[\"k\",8],"
          "[\"k\",9],[\"k\",10],[\"k\",11],[\"k\",12],[\"k\",13],[\"k\",14],[\"k\",15],[\"k\",16],"
          "\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\",\"+\","
          "\"neg\",\"neg\"]",
          VP_RPN_ERR_TOO_LONG, 32 },
    };
    size_t i;

    for (i = 0; i < N(cases); i++) {
        vp_rpn_err_t e = dec(cases[i].json, NULL, c, &n, &at);
        VT_CHECK_MSG(e == cases[i].err, "%s -> %d (%s)", cases[i].json, (int)e, vp_rpn_err_str(e));
        if (e == cases[i].err) VT_CHECK_MSG(at == cases[i].at, "%s at %u", cases[i].json, (unsigned)at);
    }
    VT_CHECK_INT(vp_rpn_from_json(NULL, NULL, c, VP_RPN_MAX_INSNS, &n, &at), VP_RPN_ERR_OPCODE);
}

int main(void)
{
    VT_RUN(test_push_byte);
    VT_RUN(test_push_raw);
    VT_RUN(test_raw_all_fields);
    VT_RUN(test_const);
    VT_RUN(test_arith);
    VT_RUN(test_clamp);
    VT_RUN(test_div0);
    VT_RUN(test_int_ops);
    VT_RUN(test_sext);
    VT_RUN(test_lut);
    VT_RUN(test_stack);
    VT_RUN(test_nonfinite);
    VT_RUN(test_opcode);
    VT_RUN(test_json_example);
    VT_RUN(test_json_opcodes);
    VT_RUN(test_json_x);
    VT_RUN(test_json_errors);
    return VT_SUMMARY("test_rpn");
}
