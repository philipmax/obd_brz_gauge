#include "rx_append_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t comparisons;
static void check(bool ok, const char *what)
{
    if (!ok) { fprintf(stderr, "RX failure after %llu comparisons: %s\n",
                       (unsigned long long)comparisons, what); exit(EXIT_FAILURE); }
}
static void feed(ra_case_t *c, const uint8_t *data, size_t len)
{
    check(ra_compare(c, data, len), "engine bytes or callback trace changed"); comparisons++;
}
static uint32_t rng(uint32_t *s)
{
    *s = *s * UINT32_C(1664525) + UINT32_C(1013904223); return *s;
}
int main(void)
{
    static const size_t offsets[] = {0,1,255,511,512};
    static const size_t lengths[] = {0,1,2,19,20,127,128,129,511,512,513,1024};
    static const uint8_t frame[] = "360 00 00 82 78 00 00 00 00\r";
    static const uint8_t stream[] = "360 00 00 82 78 00 00 00 00\r\n"
        "1: 360 00 00 82 78 00 00 00 00\r140 00 00 00 00 00 00 33 00\n"
        "1:360 00 00 82 78 00 00 00 00\rCAN ERROR\r?\rSTOPPED\r>";
    uint8_t prefix[512], data[1024], *full, *overflow;
    size_t rx_cap = ra_rx_capacity();
    unsigned mode, prompt, byte, oi, li, n, step;
    size_t split;
    uint32_t seed = 193;
    ra_observation_t o;
    ra_case_t *c = ra_create();
    full = malloc(rx_cap);
    overflow = malloc(rx_cap + 1);
    check(full != NULL && overflow != NULL, "allocate RX capacity probes");
    check(c != NULL, "create monitor profile");
    /* Exhaust all byte values with realistic transport sizes and both flags. */
    memset(prefix, '7', sizeof(prefix));
    for (mode=0; mode<5; mode++) for (prompt=0; prompt<2; prompt++)
    for (oi=0; oi<sizeof(offsets)/sizeof(offsets[0]); oi++)
    for (li=0; li<sizeof(lengths)/sizeof(lengths[0]); li++) for (byte=0; byte<256; byte++) {
        memset(data, (int)byte, sizeof(data));
        ra_reset(c, prefix, offsets[oi], mode, prompt != 0, rng(&seed));
        feed(c, data, lengths[li]);
    }
    /* Complete valid CAN frames at every chunk split, including CR/LF and >. */
    for (split=0; split<sizeof(stream); split++) {
        ra_reset(c, NULL, 0, 2, false, (uint32_t)split);
        feed(c, stream, split); feed(c, stream+split, sizeof(stream)-1-split);
        ra_observe(c, &o);
        check(o.frames == 3 && o.used == 0 && o.prompt && o.refused,
              "split monitor stream must consume three frames and refusal/prompt");
    }
    ra_reset(c, NULL, 0, 2, false, UINT32_MAX);
    feed(c, frame, sizeof(frame)-1); ra_observe(c, &o);
    check(o.frames == 1 && o.used == 0 && o.writes == 2 && o.clocks == 1 &&
          o.coolant == 80 && o.oil == 90, "decoded CAN writes must stay 80C/90C");
    /* Null data returns before flushing; zero-length nonnull can flush full RX. */
    memset(full, '7', rx_cap);
    ra_reset(c, full, rx_cap, 2, false, 0);
    feed(c, NULL, 1024); ra_observe(c, &o);
    check(o.used == rx_cap && o.clocks == 0, "null data must not flush");
    feed(c, data, 0); ra_observe(c, &o);
    check(o.used == 0 && o.clocks == 1, "zero-length nonnull full buffer must flush");
    /* Scan every input byte after capacity; preserve end-of-call monitor flush. */
    memset(overflow, '7', rx_cap + 1); overflow[rx_cap] = '>';
    ra_reset(c, NULL, 0, 1, false, 0); feed(c, overflow, rx_cap + 1); ra_observe(c, &o);
    check(o.used == rx_cap && o.prompt && o.clocks == 0, "prompt beyond capacity must survive");
    ra_reset(c, NULL, 0, 2, false, 0); feed(c, overflow, rx_cap + 1); ra_observe(c, &o);
    check(o.used == 0 && o.prompt && o.frames == 0 && o.clocks == 1,
          "monitor overflow must consume once at call end");
    /* Leading NULs and empty CR/LF lines cannot suppress a later real frame. */
    memset(data, 0, 11); memcpy(data+11, "\r\n", 2);
    memcpy(data+13, frame, sizeof(frame)-1);
    ra_reset(c, NULL, 0, 2, false, 1); feed(c, data, 13+sizeof(frame)-1); ra_observe(c, &o);
    check(o.frames == 1 && o.used == 0 && o.writes == 2, "NUL filtering and empty lines");
    /* Stream replay crosses tails, overflow and timestamp wrap repeatedly. */
    for (n=0; n<256; n++) {
        ra_reset(c, NULL, 0, n%5, (n&1u)!=0, UINT32_MAX-n);
        for (step=0; step<256; step++) {
            size_t len = rng(&seed)%sizeof(data), i;
            for (i=0; i<len; i++) data[i] = (uint8_t)(rng(&seed)>>24);
            feed(c, data, len);
        }
    }
    /* Public null-engine guard is exercised in the private probe too. */
    check(ra_compare(NULL, data, 1), "null engine guard"); comparisons++;
    printf("RX append: %llu complete engine/callback comparisons; engine=%zu bytes\n",
           (unsigned long long)comparisons, ra_engine_size());
    free(full);
    free(overflow);
    ra_destroy(c); return EXIT_SUCCESS;
}
