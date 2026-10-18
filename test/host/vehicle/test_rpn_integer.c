/* Old mathematical conversion rules are an independent reference. */
#include "rpn_integer_probe.h"
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RI_HAVE_BASELINE
vp_rpn_err_t ri_baseline_eval(const vp_rpn_insn_t *, size_t, const uint8_t *, size_t,
                              const vp_rpn_env_t *, float *);
#endif
static uint64_t conversions, programs;
static uint32_t bits(float f) { uint32_t v; memcpy(&v, &f, sizeof(v)); return v; }
static float value(uint32_t v) { float f; memcpy(&f, &v, sizeof(f)); return f; }
static bool old_u32(float f, uint32_t *out)
{
    double d = (double)f;
    if (!isfinite(d) || d <= -2147483649.0 || d >= 4294967296.0) return false;
    *out = (uint32_t)(int64_t)d; return true;
}
static bool old_shift(float f, uint32_t *out)
{
    double d = (double)f;
    if (!isfinite(d) || d <= -1.0 || d >= 32.0) return false;
    *out = (uint32_t)d; return true;
}
static void check_conversion(uint32_t v)
{
    float f = value(v); uint32_t x = 0xA57319CEu, y = x;
    bool a = old_u32(f, &x), b = ri_to_u32(f, &y);
    if (a != b || x != y) {
        fprintf(stderr, "u32 mismatch input=%08" PRIx32 " accepted=%d/%d output=%08" PRIx32 "/%08" PRIx32 "\n", v,a,b,x,y); exit(1);
    }
    x = y = 0xA57319CEu; a = old_shift(f, &x); b = ri_to_shift(f, &y);
    if (a != b || x != y) {
        fprintf(stderr, "shift mismatch input=%08" PRIx32 " accepted=%d/%d output=%08" PRIx32 "/%08" PRIx32 "\n",v,a,b,x,y); exit(1);
    }
    conversions++;
}
/* Build every finite binary32 value from legal constants. Powers of two keep
 * each intermediate exact, including the operands outside the integer range. */
static size_t operand(vp_rpn_insn_t *code, float f)
{
    size_t n = 0; int exponent = 0;
    if (isfinite(f) && fabsf(f) > (float)VP_RPN_MAX_CONST) f = frexpf(f, &exponent);
    code[n++] = (vp_rpn_insn_t){.op=VP_OP_PUSH_CONST,.u.k=f};
    while (exponent > 0) {
        int shift = exponent > 16 ? 16 : exponent;
        code[n++] = (vp_rpn_insn_t){.op=VP_OP_PUSH_CONST,.u.k=ldexpf(1.0f,shift)};
        code[n++] = (vp_rpn_insn_t){.op=VP_OP_MUL}; exponent -= shift;
    }
    return n;
}
static void check_program(uint8_t op, float a, float b)
{
    vp_rpn_insn_t code[VP_RPN_MAX_INSNS]; size_t n = operand(code, a);
    uint32_t ua=0, ub=0; float expected=value(0xC568E000u), actual=expected;
    bool unary = op == VP_OP_SEXT8 || op == VP_OP_SEXT16;
    vp_rpn_err_t want=VP_RPN_OK, got;
    if (!unary) n += operand(code+n,b);
    code[n++] = (vp_rpn_insn_t){.op=op};
    if (!isfinite(a) || (!unary && !isfinite(b))) want=VP_RPN_ERR_CONST;
    else if (!old_u32(a,&ua) || (!unary && !((op==VP_OP_SHR || op==VP_OP_SHL) ? old_shift(b,&ub) : old_u32(b,&ub)))) want=VP_RPN_ERR_INT;
    else {
        switch(op) {
        case VP_OP_AND: expected=(float)(ua&ub); break;
        case VP_OP_OR: expected=(float)(ua|ub); break;
        case VP_OP_SHR: expected=(float)(ua>>ub); break;
        case VP_OP_SHL: expected=(float)(uint32_t)(ua<<ub); break;
        case VP_OP_SEXT8: ua&=255u; expected=(float)((ua&128u)?(int32_t)ua-256:(int32_t)ua); break;
        case VP_OP_SEXT16: ua&=65535u; expected=(float)((ua&32768u)?(int32_t)ua-65536:(int32_t)ua); break;
        default: abort();
        }
    }
    got=ri_eval(code,n,NULL,0,NULL,&actual);
    if(got!=want || bits(actual)!=bits(expected)) {
        fprintf(stderr,"eval mismatch op=%u a=%08" PRIx32 " b=%08" PRIx32 " status=%d/%d result=%08" PRIx32 "/%08" PRIx32 "\n",op,bits(a),bits(b),want,got,bits(expected),bits(actual)); exit(1);
    }
#ifdef RI_HAVE_BASELINE
    actual=value(0xC568E000u); got=ri_baseline_eval(code,n,NULL,0,NULL,&actual);
    if(got!=want || bits(actual)!=bits(expected)) { fputs("baseline eval mismatch\n",stderr); exit(1); }
#endif
    programs++;
}
int main(int argc, char **argv)
{
    static const uint32_t boundaries[]={
        0,0x80000000u,1,0x80000001u,0x007FFFFFu,0x807FFFFFu,0x00800000u,0x80800000u,
        0x3F800000u,0xBF800000u,0x42000000u,0xC2000000u,0x4F000000u,0xCF000000u,
        0x4F800000u,0xCF800000u,0x7F800000u,0xFF800000u,0x7FC00000u,0xFFC00000u,
        0x7FA00001u,0xFFA00001u,0x7FFFFFFFu,0xFFFFFFFFu,0x437F0000u,0x477FFF00u};
    static const uint8_t ops[]={VP_OP_AND,VP_OP_OR,VP_OP_SHR,VP_OP_SHL,VP_OP_SEXT8,VP_OP_SEXT16};
    static const float rhs[]={-1.0f,-0.75f,-0.0f,0.0f,0.75f,1.0f,7.0f,15.0f,31.0f,31.75f,32.0f,255.0f,65535.0f};
    uint32_t rng=0x716AF293u; size_t i,j,k; uint64_t v;
    if(sizeof(float)!=4 || FLT_RADIX!=2 || FLT_MANT_DIG!=24 || FLT_MAX_EXP!=128) return 2;
    for(i=0;i<sizeof(boundaries)/sizeof(boundaries[0]);i++) for(j=0;j<65;j++) {
        uint32_t pattern=boundaries[i]+(uint32_t)j-32u;
        check_conversion(pattern);
        for(k=0;k<sizeof(ops)/sizeof(ops[0]);k++) check_program(ops[k],value(pattern),rhs[j%(sizeof(rhs)/sizeof(rhs[0]))]);
    }
    for(i=0;i<1000000;i++) {
        rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; check_conversion(rng);
        if(i<50000) for(k=0;k<sizeof(ops)/sizeof(ops[0]);k++) {
            check_program(ops[k],value(rng),rhs[i%(sizeof(rhs)/sizeof(rhs[0]))]);
            check_program(ops[k],rhs[i%(sizeof(rhs)/sizeof(rhs[0]))],value(rng));
        }
    }
    if(argc==2 && !strcmp(argv[1],"--exhaustive")) {
        for(v=0;v<=UINT32_MAX;v++) {
            check_conversion((uint32_t)v);
            if((v&UINT64_C(0x1FFFFFFF))==UINT64_C(0x1FFFFFFF)) {
                fprintf(stderr,"checked binary32 patterns: %" PRIu64 "/4294967296\n",v+1); fflush(stderr);
            }
        }
    } else if(argc!=1) return 2;
    printf("integer conversions: %" PRIu64 " patterns; full formulas: %" PRIu64 "; PASS\n",conversions,programs);
    return 0;
}
