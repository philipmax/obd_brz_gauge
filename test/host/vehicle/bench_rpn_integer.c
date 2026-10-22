#define _POSIX_C_SOURCE 200809L
#include "rpn_integer_probe.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
vp_rpn_err_t ri_baseline_eval(const vp_rpn_insn_t *, size_t, const uint8_t *, size_t,
                              const vp_rpn_env_t *, float *);
typedef vp_rpn_err_t (*eval_fn)(const vp_rpn_insn_t *,size_t,const uint8_t *,size_t,const vp_rpn_env_t *,float *);
#define BYTE(i) {.op=VP_OP_PUSH_BYTE,.u.arg=(i)}
#define RAW(f) {.op=VP_OP_PUSH_RAW,.bits=32,.flags=(f),.u.arg=0}
#define K(x) {.op=VP_OP_PUSH_CONST,.u.k=(x)}
#define OP(x) {.op=(x)}
#define SAMPLES 7u
#define ITERATIONS 500000u
static uint8_t input[256][4];
static uint64_t ns(void)
{
    struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) abort();
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static uint64_t measure(eval_fn fn,const vp_rpn_insn_t *code,size_t n,unsigned count,uint32_t *sum)
{
    uint64_t begin=ns(); uint32_t s=0; unsigned i;
    for(i=0;i<count;i++) {
        float out=-12345.0f; uint32_t v;
        vp_rpn_err_t e=fn(code,n,input[i&255u],4,NULL,&out);
        memcpy(&v,&out,sizeof(v)); s+=v+(uint32_t)e;
    }
    *sum=s; return ns()-begin;
}
static int compare(const void *a,const void *b)
{
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b; return (x>y)-(x<y);
}
int main(void)
{
    static const struct {const char *name;size_t n;vp_rpn_insn_t code[8];} cases[]={
#ifdef RI_RAW_FLOAT_BENCHMARK
        {"raw8_unsigned_le",1,{{.op=VP_OP_PUSH_RAW,.bits=8}}},
        {"raw8_signed_le",1,{{.op=VP_OP_PUSH_RAW,.bits=8,.flags=VP_RAW_SIGNED}}},
        {"raw16_unsigned_be",1,{{.op=VP_OP_PUSH_RAW,.bits=16,.flags=VP_RAW_BE}}},
        {"raw16_signed_be",1,{{.op=VP_OP_PUSH_RAW,.bits=16,.flags=VP_RAW_BE|VP_RAW_SIGNED}}},
        {"raw21_unsigned_unaligned",1,{{.op=VP_OP_PUSH_RAW,.bits=21,.u.arg=3}}},
        {"raw21_signed_unaligned",1,{{.op=VP_OP_PUSH_RAW,.bits=21,.flags=VP_RAW_SIGNED,.u.arg=3}}},
        {"raw32_unsigned_le",1,{RAW(0)}},
        {"raw32_signed_le",1,{RAW(VP_RAW_SIGNED)}},
        {"raw32_unsigned_be",1,{RAW(VP_RAW_BE)}},
        {"raw32_signed_be",1,{RAW(VP_RAW_BE|VP_RAW_SIGNED)}},
        {"raw32_short_error",1,{{.op=VP_OP_PUSH_RAW,.bits=32,.u.arg=1}}},
        {"raw_width_error",1,{{.op=VP_OP_PUSH_RAW,.bits=33}}},
        {"raw_start_error",1,{{.op=VP_OP_PUSH_RAW,.bits=8,.u.arg=65536}}},
        {"raw_temperature",5,{{.op=VP_OP_PUSH_RAW,.bits=16,.flags=VP_RAW_BE|VP_RAW_SIGNED},K(0.5f),OP(VP_OP_MUL),K(40),OP(VP_OP_SUB)}},
        {"byte_control",1,{BYTE(0)}},
        {"rpm_control",7,{BYTE(0),K(256),OP(VP_OP_MUL),BYTE(1),OP(VP_OP_ADD),K(4),OP(VP_OP_DIV)}}
#else
        {"byte_and",3,{BYTE(0),K(15),OP(VP_OP_AND)}},
        {"raw_or",3,{RAW(0),K(255),OP(VP_OP_OR)}},
        {"raw_shr",3,{RAW(0),K(13),OP(VP_OP_SHR)}},
        {"signed_shl",3,{RAW(VP_RAW_SIGNED),K(7),OP(VP_OP_SHL)}},
        {"negative_shift_fraction",3,{RAW(VP_RAW_SIGNED),K(-0.75f),OP(VP_OP_SHR)}},
        {"invalid_shift",3,{RAW(0),K(32),OP(VP_OP_SHL)}},
        {"raw_sext8",2,{RAW(0),OP(VP_OP_SEXT8)}},
        {"raw_sext16",2,{RAW(0),OP(VP_OP_SEXT16)}},
        {"fraction_and",5,{BYTE(0),K(0.25f),OP(VP_OP_MUL),K(15),OP(VP_OP_AND)}},
        {"rpm_control",7,{BYTE(0),K(256),OP(VP_OP_MUL),BYTE(1),OP(VP_OP_ADD),K(4),OP(VP_OP_DIV)}},
        {"byte_control",1,{BYTE(0)}},
        {"raw_control",1,{RAW(0)}}
#endif
    };
    size_t i,j; unsigned sample; uint32_t rng=0x7359AB13u;
    for(i=0;i<256;i++) for(j=0;j<4;j++) {
        rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;input[i][j]=(uint8_t)rng;
    }
    puts("case,iterations,baseline_ns,production_ns,baseline_to_production,checksum");
    for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        uint64_t old[SAMPLES],current[SAMPLES];uint32_t x,y;
        for(j=0;j<256;j++) {
            float a=-12345.0f,b=a;uint32_t av,bv;
            vp_rpn_err_t ae=ri_baseline_eval(cases[i].code,cases[i].n,input[j],4,NULL,&a);
            vp_rpn_err_t be=ri_eval(cases[i].code,cases[i].n,input[j],4,NULL,&b);
            memcpy(&av,&a,sizeof(av));memcpy(&bv,&b,sizeof(bv));if(ae!=be || av!=bv) abort();
        }
        measure(ri_baseline_eval,cases[i].code,cases[i].n,10000,&x);
        measure(ri_eval,cases[i].code,cases[i].n,10000,&y);if(x!=y) abort();
        for(sample=0;sample<SAMPLES;sample++) {
            if(sample&1u) {
                current[sample]=measure(ri_eval,cases[i].code,cases[i].n,ITERATIONS,&y);
                old[sample]=measure(ri_baseline_eval,cases[i].code,cases[i].n,ITERATIONS,&x);
            } else {
                old[sample]=measure(ri_baseline_eval,cases[i].code,cases[i].n,ITERATIONS,&x);
                current[sample]=measure(ri_eval,cases[i].code,cases[i].n,ITERATIONS,&y);
            }
            if(x!=y) abort();
        }
        qsort(old,SAMPLES,sizeof(old[0]),compare);qsort(current,SAMPLES,sizeof(current[0]),compare);
        printf("%s,%u,%.2f,%.2f,%.3f,%" PRIu32 "\n",cases[i].name,ITERATIONS,
               (double)old[SAMPLES/2]/ITERATIONS,(double)current[SAMPLES/2]/ITERATIONS,
               (double)old[SAMPLES/2]/(double)current[SAMPLES/2],x);
    }
    return 0;
}
