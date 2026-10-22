/* Real page acquisition vs the frozen pre-identity path. Source buffers live
 * until all cache references are released, as the oracle fixture. The new path owns an SD copy.
 * Shared geometry/conversion helpers are tested by their separate suites. */
#define _POSIX_C_SOURCE 200809L
#define TE_PAGE_ALLOC_CUSTOM
#define main page_tests_main
#include "test_page.c"
#undef main
#include "acquire_reference.inc"

typedef union {
    struct { size_t size; } s;
    long double align_ld;
    void *align_ptr;
} ta_header_t;
static struct {
    size_t live, peak, calls, pixels, entries, fail_size, fail_call;
    int fail_kind;
    bool trace;
} ta;
static size_t ta_free_bytes = 8u * 1024u * 1024u;

static void *ta_alloc(size_t n, int kind, bool zero)
{
    if (ta.trace) {
        ta.calls++;
        if (kind == 1) ta.pixels++; else ta.entries++;
        if ((ta.fail_call && ta.calls == ta.fail_call) ||
            (ta.fail_kind == kind && (!ta.fail_size || ta.fail_size == n))) return NULL;
    }
    if (n > SIZE_MAX - sizeof(ta_header_t)) return NULL;
    ta_header_t *h = malloc(sizeof(*h) + n);
    if (!h) return NULL;
    h->s.size = n;
    if (zero) memset(h + 1, 0, n);
    if (ta.trace) {
        ta.live += n;
        if (ta.live > ta.peak) ta.peak = ta.live;
    }
    return h + 1;
}
void ta_heap_free(void *p)
{
    if (!p) return;
    ta_header_t *h = (ta_header_t *)p - 1;
    if (ta.trace) { TH_CHECK(ta.live >= h->s.size); ta.live -= h->s.size; }
    free(h);
}
void *ta_heap_malloc(size_t n) { return ta_alloc(n, 2, false); }
void *ta_heap_calloc(size_t n, size_t z)
{ return z && n > SIZE_MAX / z ? NULL : ta_alloc(n * z, 2, true); }
size_t ta_heap_free_size(void) { return ta_free_bytes; }
void *te_alloc_cb(size_t n, void *ctx) { (void)ctx; return ta_alloc(n, 1, false); }
void *te_calloc_cb(size_t n, size_t z, void *ctx)
{ (void)ctx; return z && n > SIZE_MAX / z ? NULL : ta_alloc(n * z, 1, true); }
void te_free_cb(void *p, void *ctx) { (void)ctx; ta_heap_free(p); }

typedef tic_entry_t *(*ta_fn)(te_build_t *, const tl_elem_t *, const te_vkey_t *, int, int *);
typedef struct {
    int status;
    bool exists, empty, owned;
    uint32_t cf, w, h;
    int32_t ox, oy;
    size_t cost, size, peak, calls, pixels;
    tic_variant_t var;
    uint8_t *data;
} ta_result_t;
static unsigned ta_cases;
static uint32_t ta_rand = 0x56781234u;
static uint32_t ta_random(void)
{ ta_rand ^= ta_rand << 13; ta_rand ^= ta_rand >> 17; ta_rand ^= ta_rand << 5; return ta_rand; }
static uint64_t ta_hash(const uint8_t *p, size_t n)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * UINT64_C(1099511628211);
    return h;
}
static uint8_t *ta_source(te_image_t *im, unsigned fmt, unsigned w, unsigned h, unsigned pattern)
{
    *im = (te_image_t){.fmt=fmt, .w=w, .h=h, .size=w*h*(fmt ? 4u : 2u)};
    im->offset = 8;
    uint8_t *storage = malloc(im->size + im->offset);
    if (!storage) abort();
    memset(storage, 0xa5, im->offset);
    uint8_t *data = storage + im->offset;
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
        size_t i = (size_t)y*w+x;
        if (!fmt) {
            uint16_t color = (uint16_t)ta_random();
            data[2*i] = (uint8_t)(color >> 8); data[2*i+1] = (uint8_t)color;
        } else {
            uint8_t *p = data+4*i;
            p[0]=(uint8_t)ta_random(); p[1]=(uint8_t)ta_random(); p[2]=(uint8_t)ta_random();
            switch (pattern) {
            case 0: p[3]=255; break;
            case 1: p[3]=0; break;
            case 2: p[3]=(uint8_t)(ta_random()%3); break;
            case 3: p[3]=(uint8_t)ta_random(); break;
            default: p[3]=(x>=w/3 && x<2*w/3+1 && y>=h/3 && y<2*h/3+1) ? 255 : 0; break;
            }
        }
    }
    return storage;
}
static ta_result_t ta_run(ta_fn fn, te_image_t *im, uint8_t *data, tl_elem_t *el,
                           te_vkey_t *key, int pol, size_t budget,
                           int fail_kind, size_t fail_size, size_t fail_call)
{
    TH_CHECK(ta.live == 0);
    memset(&ta, 0, sizeof(ta)); ta.trace=true;
    ta.fail_kind=fail_kind; ta.fail_size=fail_size; ta.fail_call=fail_call;
    te_ctx_t ctx = {.map=data, .file_size=im->offset+im->size, .n_images=1, .manifest_crc=0x12345678};
    g_te = &ctx;
    ctx.images[0] = *im;
    tic_init(&ctx.cache);
    te_build_t b = {.c=&ctx, .budget=budget};
    ta_result_t out = {0};
    tic_entry_t *e = fn(&b, el, key, pol, &out.status);
    out.exists = e != NULL;
    out.peak=ta.peak; out.calls=ta.calls; out.pixels=ta.pixels;
    if (e) {
        out.empty=e->empty; out.owned=e->owned != NULL; out.cost=e->cost;
        out.cf=e->dsc.header.cf; out.w=e->dsc.header.w; out.h=e->dsc.header.h;
        out.ox=e->ox; out.oy=e->oy; out.size=e->dsc.data_size; out.var=e->var;
        if (out.size) { out.data=malloc(out.size); memcpy(out.data, e->dsc.data, out.size); }
        TH_CHECK(e->refs == 1 && ctx.cache.count == 1);
        TH_CHECK(ctx.cache.used == e->cost && ctx.cache.pinned == e->cost && b.cost == e->cost);
        if (!out.owned && !out.empty) TH_CHECK(e->dsc.data == data + im->offset);
        size_t calls=ta.calls, live=ta.live;
        int st;
        tic_entry_t *hit=fn(&b, el, key, TE_POL_REQUIRED, &st);
        TH_CHECK(hit == e && st == TE_ACQ_OK && e->refs == 2);
        TH_CHECK(ta.calls == calls && ta.live == live);
        TH_CHECK(b.cost == e->cost * 2 && ctx.cache.pinned == e->cost);
        if (e->cost) {
            size_t budget_before=b.budget, cost_before=b.cost;
            b.budget=0;
            TH_CHECK(fn(&b, el, key, TE_POL_BUDGET, &st) == NULL && st == TE_ACQ_BUDGET);
            TH_CHECK(e->refs == 2 && b.cost == cost_before && ta.calls == calls);
            b.budget=budget_before;
        }
        tic_clear(&ctx.cache);
        TH_CHECK(e->doomed && ctx.cache.count == 1 && ta.live == live);
        tic_release(&ctx.cache, hit);
        TH_CHECK(e->refs == 1 && ctx.cache.count == 1 && ta.live == live);
        tic_release(&ctx.cache, e);
    }
    tic_clear(&ctx.cache);
    TH_CHECK(ctx.cache.used == 0 && ctx.cache.pinned == 0 && ctx.cache.count == 0);
    TH_CHECK(ta.live == 0);
    return out;
}
static void ta_same(const ta_result_t *a, const ta_result_t *b)
{
    TH_CHECK(a->status == b->status && a->exists == b->exists);
    if (!a->exists || !b->exists) return;
    TH_CHECK(a->empty == b->empty && a->cf == b->cf && a->w == b->w && a->h == b->h);
    TH_CHECK(a->ox == b->ox && a->oy == b->oy && a->size == b->size);
    TH_CHECK(!memcmp(&a->var, &b->var, sizeof(a->var)));
    if (a->size == b->size && a->size) TH_CHECK(!memcmp(a->data, b->data, a->size));
}
static void ta_pair(unsigned fmt, unsigned w, unsigned h, unsigned dw, unsigned dh,
                    unsigned pattern, bool tint, double px, double py, bool image)
{
    te_image_t im;
    uint8_t *data=ta_source(&im, fmt, w, h, pattern);
    uint64_t hash=ta_hash(data, im.size + im.offset);
    tl_elem_t el={.kind=image ? TR_KIND_IMAGE : TR_KIND_POINTER, .width=dw, .height=dh,
                  .x=160, .y=160, .pivot_x=px, .pivot_y=py};
    te_vkey_t key={.asset=0, .has_tint=tint, .tint=0x953bed};
    ta_result_t old=ta_run(ta_reference, &im, data, &el, &key, TE_POL_REQUIRED, SIZE_MAX/2, 0,0,0);
    ta_result_t cur=ta_run(te_acquire, &im, data, &el, &key, TE_POL_REQUIRED, SIZE_MAX/2, 0,0,0);
    ta_same(&old,&cur);
    TH_CHECK(cur.status == TE_ACQ_OK);
    TH_CHECK(hash == ta_hash(data,im.size + im.offset));
    if (!cur.empty) {
        TH_CHECK(cur.owned && cur.cost == cur.size);
        TH_CHECK(cur.data != data + im.offset);
    }
    TH_CHECK(te_est_cost(&el,&im,&key) >= cur.cost);
    free(old.data); free(cur.data); free(data); ta_cases++;
}
static void ta_matrix(void)
{
    static const unsigned dims[][4]={{1,1,1,1},{1,37,1,37},{53,1,53,1},{13,19,13,19},
        {90,150,90,150},{360,360,360,360},{2047,1,2047,1},{1,2047,1,2047},
        {13,19,7,31},{7,31,13,19},{13,19,13,7},{13,19,23,19}};
    static const double piv[][2]={{50,50},{0,0},{100,100},{-100,200},{200,-100},{35,65}};
    for (unsigned f=0; f<2; f++) for (unsigned d=0; d<sizeof(dims)/sizeof(dims[0]); d++)
      for (unsigned p=0; p<(f ? 5u : 1u); p++) for (unsigned t=0; t<2; t++)
        for (unsigned v=0; v<sizeof(piv)/sizeof(piv[0]); v++) {
            ta_pair(f,dims[d][0],dims[d][1],dims[d][2],dims[d][3],p,t,piv[v][0],piv[v][1],false);
        }
    for (unsigned f=0;f<2;f++) for (unsigned p=0;p<(f ? 5u : 1u);p++)
      for (unsigned t=0;t<2;t++) ta_pair(f,13,19,9,13,p,t,50,50,true);
}
static void ta_extra(void)
{
    /* Every possible RGB565 code survives expansion and re-quantization. */
    te_image_t im;
    uint8_t *data=ta_source(&im,0,256,256,0);
    for (unsigned i=0;i<65536;i++) {
        data[im.offset+2*i]=(uint8_t)(i>>8); data[im.offset+2*i+1]=(uint8_t)i;
    }
    tl_elem_t el={.kind=TR_KIND_POINTER,.width=256.49,.height=256.49,.pivot_x=37.5,.pivot_y=65.5};
    for(unsigned t=0;t<2;t++) {
        te_vkey_t key={.asset=0,.has_tint=t,.tint=0x953bed};
        uint64_t hash=ta_hash(data,im.size+im.offset);
        ta_result_t old=ta_run(ta_reference,&im,data,&el,&key,TE_POL_REQUIRED,SIZE_MAX/2,0,0,0);
        ta_result_t cur=ta_run(te_acquire,&im,data,&el,&key,TE_POL_REQUIRED,SIZE_MAX/2,0,0,0);
        ta_same(&old,&cur); TH_CHECK(cur.status == TE_ACQ_OK && hash == ta_hash(data,im.size+im.offset));
        free(old.data);free(cur.data);ta_cases++;
    }
    free(data);
    for (unsigned i=0;i<128;i++) {
        unsigned w=1+ta_random()%50,h=1+ta_random()%50;
        unsigned dw=(i&1) ? w : 1+ta_random()%50, dh=(i&1) ? h : 1+ta_random()%50;
        ta_pair(i%2,w,h,dw,dh,i%5,(i&2)!=0,(double)(ta_random()%301)-100,
                (double)(ta_random()%301)-100,false);
    }
}
static void ta_failures(void)
{
    for (unsigned f=0; f<2; f++) for (unsigned tint=0; tint<2; tint++) {
        te_image_t im; uint8_t *data=ta_source(&im,f,13,19,0);
        uint64_t hash=ta_hash(data,im.size+im.offset);
        tl_elem_t el={.kind=TR_KIND_POINTER,.width=13,.height=19,.pivot_x=50,.pivot_y=50};
        te_vkey_t key={.asset=0,.has_tint=tint,.tint=0x953bed};
        for (int pol=TE_POL_REQUIRED; pol<=TE_POL_DECOR; pol++) {
            ta_result_t cur=ta_run(te_acquire,&im,data,&el,&key,pol,0,0,0,0);
            TH_CHECK(cur.status == (pol==TE_POL_REQUIRED ? TE_ACQ_OK : TE_ACQ_BUDGET));
            free(cur.data);
        }
        ta_free_bytes=TE_PSRAM_RESERVE;
        ta_result_t cur=ta_run(te_acquire,&im,data,&el,&key,TE_POL_DECOR,10000,0,0,0);
        TH_CHECK(cur.status == TE_ACQ_BUDGET); free(cur.data);
        ta_free_bytes=8u*1024u*1024u;
        /* Persistent failure of input, output, or entry leaves no ownership. */
        for (unsigned which=0; which<3; which++) {
            size_t size=which==0 ? im.size : which==1 ? 13u*19u*2u : sizeof(tic_entry_t);
            cur=ta_run(te_acquire,&im,data,&el,&key,TE_POL_REQUIRED,10000,
                       which==1 ? 1 : 2,size,0);
            TH_CHECK(cur.status == TE_ACQ_NOMEM); free(cur.data);
        }
        te_read_fail=true;
        cur=ta_run(te_acquire,&im,data,&el,&key,TE_POL_REQUIRED,10000,0,0,0);
        TH_CHECK(cur.status == TE_ACQ_ERR); free(cur.data); te_read_fail=false;
        el.width=17; el.height=23;
        cur=ta_run(te_acquire,&im,data,&el,&key,TE_POL_REQUIRED,10000,0,0,0);
        size_t calls=cur.calls; free(cur.data);
        for (size_t i=1; i<=calls; i++) {
            cur=ta_run(te_acquire,&im,data,&el,&key,TE_POL_REQUIRED,10000,0,0,i);
            TH_CHECK(cur.status == TE_ACQ_OK || cur.status == TE_ACQ_NOMEM);
            free(cur.data);
        }
        el.width=13; el.height=19;
        for (unsigned invalid=0; invalid<5; invalid++) {
            te_image_t bad=im; tl_elem_t be=el;
            if (invalid==0) bad.size--;
            if (invalid==1) bad.fmt=9;
            if (invalid==2) be.width=0;
            if (invalid==3) be.height=TPX_MAX_DIM+1;
            if (invalid==4) { bad.w=0; bad.size=0; }
            cur=ta_run(te_acquire,&bad,data,&be,&key,TE_POL_REQUIRED,10000,0,0,0);
            TH_CHECK(!cur.exists); free(cur.data);
        }
        TH_CHECK(hash==ta_hash(data,im.size+im.offset)); free(data);
    }
}
static uint64_t ta_time(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) abort();
    return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
}
static volatile uint64_t ta_sink;
static double ta_measure(ta_fn fn, te_image_t *im, uint8_t *data, tl_elem_t *el,te_vkey_t *key)
{
    te_ctx_t ctx={.map=data,.file_size=im->offset+im->size,.n_images=1,.manifest_crc=0x12345678};
    g_te = &ctx;
    ctx.images[0] = *im;
    te_build_t b={.c=&ctx,.budget=SIZE_MAX/2};
    unsigned n=0;uint64_t start=ta_time(),end;
    do {
        tic_init(&ctx.cache);b.cost=0;int st;
        tic_entry_t *e=fn(&b,el,key,TE_POL_REQUIRED,&st);
        if (!e || st!=TE_ACQ_OK) abort();
        ta_sink+=e->dsc.data_size+e->ox+e->oy;
        tic_release(&ctx.cache,e);tic_clear(&ctx.cache);n++;
        end=ta_time();
    } while (end-start<UINT64_C(20000000));
    return (double)(end-start)/n;
}
static int ta_double_cmp(const void *a,const void *b)
{ double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y); }
static void ta_benchmark(void)
{
    static const unsigned dims[][4]={{90,150,90,150},{180,300,180,300},
        {360,360,360,360},{90,150,180,300},{180,300,90,150}};
    puts("fmt,tint,sw,sh,dw,dh,old_ns,new_ns,ratio,old_peak,new_peak,old_allocs,new_allocs,old_cost,new_cost,old_samples_ns,new_samples_ns");
    for (unsigned f=0;f<2;f++) for (unsigned t=0;t<2;t++) for (unsigned d=0;d<5;d++) {
        te_image_t im;uint8_t *data=ta_source(&im,f,dims[d][0],dims[d][1],4);
        tl_elem_t el={.kind=TR_KIND_POINTER,.width=dims[d][2],.height=dims[d][3],.pivot_x=0,.pivot_y=100};
        te_vkey_t key={.asset=0,.has_tint=t,.tint=0x953bed};
        ta_result_t old=ta_run(ta_reference,&im,data,&el,&key,TE_POL_REQUIRED,SIZE_MAX/2,0,0,0);
        ta_result_t cur=ta_run(te_acquire,&im,data,&el,&key,TE_POL_REQUIRED,SIZE_MAX/2,0,0,0);
        ta_same(&old,&cur);ta.trace=false;
        double a[7],b[7];
        for(unsigned i=0;i<7;i++) {
            if (i&1) {b[i]=ta_measure(te_acquire,&im,data,&el,&key);a[i]=ta_measure(ta_reference,&im,data,&el,&key);}
            else {a[i]=ta_measure(ta_reference,&im,data,&el,&key);b[i]=ta_measure(te_acquire,&im,data,&el,&key);}
        }
        double raw_a[7],raw_b[7];memcpy(raw_a,a,sizeof(a));memcpy(raw_b,b,sizeof(b));
        qsort(a,7,sizeof(double),ta_double_cmp);qsort(b,7,sizeof(double),ta_double_cmp);
        printf("%s,%u,%u,%u,%u,%u,%.3f,%.3f,%.6f,%zu,%zu,%zu,%zu,%zu,%zu",
          f ? "rgba" : "rgb565",t,im.w,im.h,dims[d][2],dims[d][3],a[3],b[3],b[3]/a[3],
          old.peak,cur.peak,old.calls,cur.calls,old.cost,cur.cost);
        for(unsigned group=0;group<2;group++) {
            printf(",");
            for(unsigned i=0;i<7;i++) printf("%s%.3f",i ? ";" : "",group ? raw_b[i] : raw_a[i]);
        }
        puts("");
        free(old.data);free(cur.data);free(data);
    }
}
int main(int argc,char **argv)
{
    lv_init();
    if(argc==2 && !strcmp(argv[1],"--benchmark")) {ta_benchmark();return th_fail ? 1 : 0;}
    ta_matrix();ta_extra();ta_failures();
    printf("acquisition equivalence: %u pixel/geometry cases\n",ta_cases);
    return th_report("theme_acquire");
}
