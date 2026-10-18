/* Test the complete evaluator against independent per-bit extraction and the
 * original int64_t-to-float conversion, including unchanged failure output. */
#include "rpn_integer_probe.h"
#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RI_HAVE_BASELINE
vp_rpn_err_t ri_baseline_eval(const vp_rpn_insn_t *, size_t, const uint8_t *, size_t,
                              const vp_rpn_env_t *, float *);
#endif
static uint64_t comparisons, valid_fields;
static uint32_t float_bits(float f) { uint32_t v; memcpy(&v,&f,sizeof(v)); return v; }
static float sentinel(void) { float f; uint32_t v=0xC568E000u; memcpy(&f,&v,sizeof(f)); return f; }
static vp_rpn_err_t reference(const uint8_t *data,size_t len,uint32_t start,
                               uint8_t width,uint8_t flags,float *out)
{
    uint32_t v=0;
    if(!data && len) return VP_RPN_ERR_ARG;
    if(!width || width>32 || start>UINT16_MAX) return VP_RPN_ERR_RAW;
    if(!data || ((uint64_t)start+width+7)/8>len) return VP_RPN_ERR_DATA;
    for(unsigned i=0;i<width;i++) {
        unsigned pos=start+i;
        if(flags&VP_RAW_BE) v=(v<<1)|((data[pos/8]>>(7u-pos%8))&1u);
        else v|=(uint32_t)((data[pos/8]>>(pos%8))&1u)<<i;
    }
    int64_t integer=((flags&VP_RAW_SIGNED) && (v&(1u<<(width-1))))
        ? (int64_t)v-((int64_t)1<<width) : (int64_t)v;
    *out=(float)integer;
    return VP_RPN_OK;
}
static void compare(const vp_rpn_insn_t *code,size_t n,const uint8_t *data,size_t len,
                    vp_rpn_err_t expected_error,float expected)
{
    float actual=sentinel();
    vp_rpn_err_t got=ri_eval(code,n,data,len,NULL,&actual);
    if(got!=expected_error || float_bits(actual)!=float_bits(expected)) {
        fprintf(stderr,"raw eval mismatch start=%" PRIu32 " width=%u flags=%u len=%zu n=%zu error=%d/%d output=%08" PRIx32 "/%08" PRIx32 "\n",
                code[0].u.arg,code[0].bits,code[0].flags,len,n,expected_error,got,float_bits(expected),float_bits(actual));
        exit(1);
    }
#ifdef RI_HAVE_BASELINE
    actual=sentinel();got=ri_baseline_eval(code,n,data,len,NULL,&actual);
    if(got!=expected_error || float_bits(actual)!=float_bits(expected)) {fputs("old raw evaluator mismatch\n",stderr);exit(1);}
#endif
    comparisons++;
}
static void field(const uint8_t *data,size_t len,uint32_t start,uint8_t width,uint8_t flags,bool derived)
{
    vp_rpn_insn_t code[5]={{.op=VP_OP_PUSH_RAW,.bits=width,.flags=flags,.u.arg=start}};
    float expected=sentinel();vp_rpn_err_t error=reference(data,len,start,width,flags,&expected);
    compare(code,1,data,len,error,expected);
    if(error==VP_RPN_OK) valid_fields++;
    if(derived) {
        code[1]=(vp_rpn_insn_t){.op=VP_OP_PUSH_CONST,.u.k=0.5f};
        code[2]=(vp_rpn_insn_t){.op=VP_OP_MUL};
        code[3]=(vp_rpn_insn_t){.op=VP_OP_PUSH_CONST,.u.k=40.0f};
        code[4]=(vp_rpn_insn_t){.op=VP_OP_SUB};
        if(error==VP_RPN_OK) {expected=expected*0.5f;expected=expected-40.0f;}
        compare(code,5,data,len,error,expected);
    }
}
/* Two complete raw32 formulas for a given bit pattern, including values that
 * round beyond UINT32_MAX as floats. Reference sign extension uses int64_t. */
static void word(uint32_t v)
{
    uint8_t data[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
    vp_rpn_insn_t code={.op=VP_OP_PUSH_RAW,.bits=32};
    compare(&code,1,data,sizeof(data),VP_RPN_OK,(float)(int64_t)v);
    code.flags=VP_RAW_SIGNED;
    int64_t signed_value=(v&0x80000000u)?(int64_t)v-INT64_C(4294967296):(int64_t)v;
    compare(&code,1,data,sizeof(data),VP_RPN_OK,(float)signed_value);
}
int main(int argc,char **argv)
{
    static const uint8_t flags[]={0,VP_RAW_BE,VP_RAW_SIGNED,VP_RAW_BE|VP_RAW_SIGNED,0x80,0x82,0xFC,0xFF};
    uint8_t *storage[9],*data[9];uint32_t rng=0x2781AF39u;size_t i,j;uint64_t v;
    if(sizeof(float)!=4 || FLT_RADIX!=2 || FLT_MANT_DIG!=24 || FLT_MAX_EXP!=128) return 2;
    for(i=0;i<9;i++) {storage[i]=malloc(i+1);if(!storage[i]) abort();data[i]=storage[i]+1;}
    for(unsigned pattern=0;pattern<12;pattern++) {
        for(i=0;i<9;i++) for(j=0;j<i;j++) {
            rng=rng*1664525u+1013904223u;
            data[i][j]=pattern==0?0:pattern==1?255:pattern==2?0xAA:pattern==3?0x55:(uint8_t)(rng>>24);
        }
        for(uint32_t start=0;start<=64;start++) for(uint8_t width=0;width<=33;width++)
            for(i=0;i<9;i++) for(j=0;j<sizeof(flags);j++)
                field(data[i],i,start,width,flags[j],(start&7u)==0);
    }
    for(i=0;i<1000000;i++) {
        rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
        for(j=0;j<8;j++) data[8][j]=(uint8_t)(rng>>(j%4)*8);
        field(data[8],8,rng%65,(uint8_t)((rng>>8)%34),(uint8_t)(rng>>16),(i&15u)==0);
    }
    /* Half-ULP and power-of-two neighborhoods, both signs via the same bits. */
    for(unsigned exponent=0;exponent<32;exponent++) {
        uint32_t center=UINT32_C(1)<<exponent;
        uint32_t spacing=exponent>23?UINT32_C(1)<<(exponent-23):1;
        for(unsigned neighbor=0;neighbor<8;neighbor++) for(unsigned offset=0;offset<513;offset++) {
            uint32_t pattern=center+neighbor*spacing+spacing/2+offset-256u;
            word(pattern);word(0u-pattern);
        }
    }
    for(i=0;i<100000;i++) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;word(rng);}
    field(NULL,0,0,8,0,true);field(NULL,4,0,8,0,true);
    field(data[8],8,UINT32_MAX,32,0,true);field(data[8],8,UINT16_MAX+1u,32,VP_RAW_SIGNED,true);
    uint8_t *far_storage=malloc(8197),*far=far_storage?far_storage+1:NULL;
    if(!far) abort();for(i=0;i<8196;i++) {rng=rng*1664525u+1013904223u;far[i]=(uint8_t)(rng>>24);}
    for(uint32_t start=UINT16_MAX-8u;start<=UINT16_MAX;start++)
        for(uint8_t width=0;width<=33;width++) for(unsigned flag=0;flag<=255;flag++) {
            size_t needed=((size_t)start+width+7)/8;
            field(far,needed,start,width,(uint8_t)flag,true);
            field(far,needed-1,start,width,(uint8_t)flag,true);
        }
    if(argc==2 && !strcmp(argv[1],"--exhaustive")) {
        for(v=0;v<=UINT32_MAX;v++) {
            word((uint32_t)v);
            if((v&UINT64_C(0x0FFFFFFF))==UINT64_C(0x0FFFFFFF)) {
                fprintf(stderr,"raw32 patterns: %" PRIu64 "/4294967296\n",v+1);fflush(stderr);
            }
        }
    } else if(argc!=1) return 2;
    for(i=0;i<9;i++) free(storage[i]);free(far_storage);
    printf("raw float: %" PRIu64 " complete formula comparisons; %" PRIu64 " valid field cases; PASS\n",comparisons,valid_fields);
    return 0;
}
