/* Paired full-image timing; use Release without sanitizers. Includes allocations. The previous path retains old tap capacity and current rounding. */
#define _POSIX_C_SOURCE 200809L
#include "resample_probe.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef tpx_err_t (*resample_fn)(const tpx_src_t *, uint16_t, uint16_t,
                               const tpx_alloc_t *, uint8_t **);
static volatile uint32_t sink;
static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static uint32_t hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
typedef struct { size_t live, peak; } memory_t;
static void *track_alloc(size_t n, void *ctx)
{
    memory_t *m = ctx;
    size_t *p = malloc(n + sizeof(size_t));
    if (!p) return NULL;
    *p = n; m->live += n;
    if (m->live > m->peak) m->peak = m->live;
    return p + 1;
}
static void track_free(void *p, void *ctx)
{
    if (!p) return;
    memory_t *m = ctx; size_t *q = (size_t *)p - 1;
    m->live -= *q; free(q);
}
static size_t peak(resample_fn fn, const tpx_src_t *src, uint16_t dw, uint16_t dh)
{
    memory_t m = {0, 0}; tpx_alloc_t allocator = {track_alloc, track_free, &m};
    uint8_t *p = NULL;
    if (fn(src, dw, dh, &allocator, &p) != TPX_OK || !p) abort();
    track_free(p, &m);
    if (m.live) abort();
    return m.peak;
}
static double run(resample_fn fn, const tpx_src_t *s, uint16_t w, uint16_t h, size_t reps)
{
    uint64_t start = ns();
    for (size_t i = 0; i < reps; i++) {
        uint8_t *p = NULL;
        if (fn(s, w, h, NULL, &p) != TPX_OK) abort();
        sink ^= p[(size_t)w * h * 4 - 1];
        free(p);
    }
    return (double)(ns() - start) / (double)reps;
}
static int order(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
int main(int argc, char **argv)
{
    bool rounding = argc == 2 && !strcmp(argv[1], "--rounding");
    if (argc > 1 && !rounding) return 2;
    /* Preserve the earlier rounding benchmark with both paths using old capacity. */
    resample_fn actual = rounding ? rs_previous : rs_actual;
    resample_fn baseline = rounding ? rs_reference : rs_previous;
    const uint16_t cases[][4] = {{360,360,180,180},{90,150,180,300},
        {180,300,90,150},{512,128,128,512},{127,129,63,65},
        {63,65,127,129},{180,300,180,300},{1,1,180,300},{2047,1,2047,2},{360,90,360,180}};
    puts("format,sw,sh,dw,dh,repetitions,reference_ns,actual_ns,ratio,checksum,previous_peak_bytes,actual_peak_bytes,saved_bytes");
    for (unsigned f = 0; f < 2; f++) for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        uint16_t sw=cases[c][0], sh=cases[c][1], dw=cases[c][2], dh=cases[c][3];
        size_t bytes=(size_t)sw*sh*(f == TPX_FMT_RGBA8888 ? 4 : 2), outbytes=(size_t)dw*dh*4;
        uint8_t *data=malloc(bytes), *a=NULL, *b=NULL;
        if (!data) return 1;
        uint32_t random=0x5431bc9du;
        for (size_t i=0;i<bytes;i++) { random^=random<<13;random^=random>>17;random^=random<<5;data[i]=(uint8_t)random; }
        tpx_src_t s={data,bytes,(tpx_fmt_t)f,sw,sh};
        if (actual(&s,dw,dh,NULL,&a)!=TPX_OK || baseline(&s,dw,dh,NULL,&b)!=TPX_OK ||
            !a || !b || memcmp(a,b,outbytes)) return 1;
        uint32_t checksum=hash(a,outbytes); free(a);free(b);
        size_t old_peak=peak(baseline,&s,dw,dh), new_peak=peak(actual,&s,dw,dh);
        if (new_peak > old_peak) return 1;
        size_t reps=4000000u/((size_t)dw*dh)+1;
        (void)run(actual,&s,dw,dh,4); (void)run(baseline,&s,dw,dh,4);
        double ta[7],tb[7];
        for (unsigned k=0;k<7;k++) {
            if (k&1) {ta[k]=run(actual,&s,dw,dh,reps);tb[k]=run(baseline,&s,dw,dh,reps);}
            else {tb[k]=run(baseline,&s,dw,dh,reps);ta[k]=run(actual,&s,dw,dh,reps);}
        }
        qsort(ta,7,sizeof(double),order);qsort(tb,7,sizeof(double),order);
        printf("%s,%u,%u,%u,%u,%zu,%.1f,%.1f,%.6f,%08" PRIx32 ",%zu,%zu,%zu\n",
               f==TPX_FMT_RGBA8888?"rgba8888":"rgb565",sw,sh,dw,dh,reps,tb[3],ta[3],ta[3]/tb[3],checksum,old_peak,new_peak,old_peak-new_peak);
        free(data);
    }
    return 0;
}
