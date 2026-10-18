#define _POSIX_C_SOURCE 200809L
#include "rx_append_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint32_t ra_baseline_feed(ra_case_t *, const uint8_t *, size_t);
bool ra_baseline_compare(ra_case_t *, const uint8_t *, size_t);
size_t ra_baseline_engine_size(void);
#define SAMPLES 7u
#define ITERATIONS 200000u
typedef uint32_t (*feed_fn)(ra_case_t *, const uint8_t *, size_t);
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static uint64_t measure(feed_fn fn, ra_case_t *c, const uint8_t *data, size_t len,
                        unsigned n, uint32_t *sum)
{
    unsigned i; uint32_t s=0; uint64_t start=ns();
    for(i=0;i<n;i++) s+=fn(c,data,len);
    *sum=s; return ns()-start;
}
static int cmp(const void *a, const void *b)
{
    uint64_t x=*(const uint64_t *)a, y=*(const uint64_t *)b; return (x>y)-(x<y);
}
int main(void)
{
    static const size_t lengths[]={0,1,20,128,512,1024};
    static const char *names[]={"ascii","nul","mixed","prompt"};
    uint8_t data[1024]; unsigned kind, li, sample; size_t i;
    ra_case_t *c=ra_create();
    if(!c || ra_engine_size()!=ra_baseline_engine_size()) abort();
    puts("case,bytes,iterations,baseline_ns,production_ns,baseline_to_production,checksum");
    for(kind=0;kind<4;kind++) for(li=0;li<sizeof(lengths)/sizeof(lengths[0]);li++) {
        uint64_t old[SAMPLES], direct[SAMPLES]; uint32_t x,y;
        for(i=0;i<sizeof(data);i++) data[i]=(uint8_t)(kind==0?'A':kind==1?0:
            kind==2?(i%7==0?0:i%31==0?'\r':'A'):(i%17==0?'>':'A'));
        ra_reset(c,NULL,0,1,false,73);
        if(!ra_compare(c,data,lengths[li])) abort();
        ra_reset(c,NULL,0,1,false,73);
        if(!ra_baseline_compare(c,data,lengths[li])) abort();
        measure(ra_baseline_feed,c,data,lengths[li],10000,&x);
        measure(ra_feed,c,data,lengths[li],10000,&y); if(x!=y) abort();
        for(sample=0;sample<SAMPLES;sample++) {
            if(sample&1u) {
                direct[sample]=measure(ra_feed,c,data,lengths[li],ITERATIONS,&y);
                old[sample]=measure(ra_baseline_feed,c,data,lengths[li],ITERATIONS,&x);
            } else {
                old[sample]=measure(ra_baseline_feed,c,data,lengths[li],ITERATIONS,&x);
                direct[sample]=measure(ra_feed,c,data,lengths[li],ITERATIONS,&y);
            }
            if(x!=y) abort();
        }
        qsort(old,SAMPLES,sizeof(old[0]),cmp); qsort(direct,SAMPLES,sizeof(direct[0]),cmp);
        printf("%s,%zu,%u,%.2f,%.2f,%.3f,%u\n",names[kind],lengths[li],ITERATIONS,
            (double)old[SAMPLES/2]/ITERATIONS,(double)direct[SAMPLES/2]/ITERATIONS,
            (double)old[SAMPLES/2]/(double)direct[SAMPLES/2],x);
    }
    ra_destroy(c); return EXIT_SUCCESS;
}
