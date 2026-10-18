/* Desktop SDK backend timing; this does not measure ESP32 ROM or radio time. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "espnow_v6.h"

uint16_t portable_espnow_crc16(const uint8_t *p, size_t n);
size_t portable_espnow_v6_encode(const espnow_snapshot_t *s, int frag, uint8_t *out, size_t cap);
bool portable_espnow_v6_decode(const uint8_t *buf, size_t len, espnow_rx_frame_t *f);
static volatile uint32_t sink;
static uint8_t data[256], frames[ESPNOW_V6_MAX_FRAGS][ESPNOW_FRAME_MAX];
static size_t lengths[ESPNOW_V6_MAX_FRAGS];
static espnow_snapshot_t snapshot;
static int frame_count;

typedef enum { CRC, ENCODE, DECODE } operation_t;
static double now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static double sample(operation_t op, bool old, size_t n)
{
    double start = now(), elapsed;
    uint64_t calls = 0;
    uint32_t sum = 0;
    uint8_t out[ESPNOW_FRAME_MAX];
    espnow_rx_frame_t rx;
    /* Each encode/decode operation covers every fragment of the snapshot. */
    do {
        for (unsigned i = 0; i < 128; i++) {
            if (op == CRC) {
                sum += old ? portable_espnow_crc16(data, n) : espnow_crc16(data, n);
            } else {
                for (int frag = 0; frag < frame_count; frag++) {
                    if (op == ENCODE) {
                        size_t len = old ? portable_espnow_v6_encode(&snapshot, frag, out, sizeof(out)) :
                                           espnow_v6_encode(&snapshot, frag, out, sizeof(out));
                        sum += (uint32_t)len + out[len - 1];
                    } else {
                        bool ok = old ? portable_espnow_v6_decode(frames[frag], lengths[frag], &rx) :
                                        espnow_v6_decode(frames[frag], lengths[frag], &rx);
                        sum += (uint32_t)ok + rx.seq + rx.n_rec;
                    }
                }
            }
        }
        calls += 128; elapsed = now() - start;
    } while (elapsed < 0.020);
    sink ^= sum;
    return elapsed * 1e9 / (double)calls;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void row(operation_t op, size_t n)
{
    double old[7], next[7], sorted[7];
    for (unsigned group = 0; group < 7; group++) {
        if (group % 2) {
            next[group] = sample(op, false, n); old[group] = sample(op, true, n);
        } else {
            old[group] = sample(op, true, n); next[group] = sample(op, false, n);
        }
    }
    memcpy(sorted, old, sizeof(old)); qsort(sorted, 7, sizeof(double), cmp); double om = sorted[3];
    memcpy(sorted, next, sizeof(next)); qsort(sorted, 7, sizeof(double), cmp); double nm = sorted[3];
    printf("%s,%zu,%d,%.6f,%.6f,%.6f,\"", op == CRC ? "crc" : op == ENCODE ? "encode" : "decode",
           n, op == CRC ? 0 : frame_count, om, nm, nm / om);
    for (unsigned i = 0; i < 7; i++) printf("%s%.6f", i ? ";" : "", old[i]);
    printf("\",\"");
    for (unsigned i = 0; i < 7; i++) printf("%s%.6f", i ? ";" : "", next[i]);
    puts("\"");
}

int main(void)
{
    const size_t crc_sizes[] = { 0, 38, 44, 104, 236, 248 };
    const unsigned counts[] = { 0, 1, 11, 33, 43, 64 };
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 73u + 19u);
    puts("operation,bytes_or_records,fragments,old_ns,new_ns,new_over_old,old_samples_ns,new_samples_ns");
    for (size_t i = 0; i < sizeof(crc_sizes) / sizeof(crc_sizes[0]); i++) {
        if (espnow_crc16(data, crc_sizes[i]) != portable_espnow_crc16(data, crc_sizes[i])) return 1;
        row(CRC, crc_sizes[i]);
    }
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        espnow_snapshot_init(&snapshot); snapshot.seq = 0x173E0Bu;
        memcpy(snapshot.name, "Bench", 6);
        for (unsigned r = 0; r < counts[i]; r++)
            if (!espnow_snapshot_put(&snapshot, (uint16_t)(ESPNOW_ID_VEH_FIRST + r), (int32_t)r * -37)) return 1;
        frame_count = espnow_v6_frame_count(&snapshot);
        for (int f = 0; f < frame_count; f++) {
            uint8_t out[ESPNOW_FRAME_MAX]; espnow_rx_frame_t rx;
            lengths[f] = espnow_v6_encode(&snapshot, f, frames[f], sizeof(frames[f]));
            size_t old = portable_espnow_v6_encode(&snapshot, f, out, sizeof(out));
            if (old != lengths[f] || !old || memcmp(out, frames[f], old)) return 1;
            if (!espnow_v6_decode(out, old, &rx) || !portable_espnow_v6_decode(out, old, &rx)) return 1;
        }
        row(ENCODE, counts[i]); row(DECODE, counts[i]);
    }
    return 0;
}
