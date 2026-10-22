/* Release-only host benchmark. The ESP_PLATFORM wrappers call ESP-IDF's Linux
 * CRC backend, not physical ROM. These timings are not ESP32 speedup claims.
 * Reference implementations live in a separate TU, without LTO.
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "obv1_format.h"
#include "vp_common.h"
#include "crc_reference.h"

#define SAMPLES 7
#define BUFFER_BYTES 288008u
#define SAMPLE_BYTES (4u * 1024u * 1024u)

typedef uint32_t (*crc_fn)(uint32_t, const void *, size_t);

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static double measure(crc_fn fn, const uint8_t *buf, size_t len, unsigned calls, uint32_t *sum)
{
    uint32_t crc = 0;
    const uint64_t start = now_ns();
    for (unsigned i = 0; i < calls; i++) crc = fn(crc, buf, len);
    const uint64_t elapsed = now_ns() - start;
    *sum = crc;
    return (double)elapsed / calls;
}

static double median(double values[SAMPLES])
{
    for (unsigned i = 1; i < SAMPLES; i++) {
        double value = values[i];
        unsigned j = i;
        while (j && value < values[j - 1]) { values[j] = values[j - 1]; j--; }
        values[j] = value;
    }
    return values[SAMPLES / 2];
}

static void scenario(const char *kind, crc_fn before, crc_fn sdk,
                     const uint8_t *buf, size_t len, unsigned off)
{
    unsigned calls = (unsigned)(SAMPLE_BYTES / len);
    if (calls < 16) calls = 16;
    uint32_t old_sum, sdk_sum;
    (void)measure(before, buf + off, len, calls, &old_sum);
    (void)measure(sdk, buf + off, len, calls, &sdk_sum);
    double old_ns[SAMPLES], sdk_ns[SAMPLES];
    for (unsigned i = 0; i < SAMPLES; i++) {
        if (i & 1u) {
            sdk_ns[i] = measure(sdk, buf + off, len, calls, &sdk_sum);
            old_ns[i] = measure(before, buf + off, len, calls, &old_sum);
        } else {
            old_ns[i] = measure(before, buf + off, len, calls, &old_sum);
            sdk_ns[i] = measure(sdk, buf + off, len, calls, &sdk_sum);
        }
        if (old_sum != sdk_sum) { fprintf(stderr, "checksum mismatch\n"); exit(1); }
    }
    double old_time = median(old_ns), sdk_time = median(sdk_ns);
    printf("%s,%zu,%u,%u,%.2f,%.2f,%.2f,%08" PRIx32 "\n",
           kind, len, off, calls, old_time, sdk_time, old_time / sdk_time, sdk_sum);
}

int main(void)
{
    static const size_t lengths[] = {60, 1024, 8192, 32768, 288000};
    uint8_t *buf = malloc(BUFFER_BYTES);
    if (!buf) return 2;
    uint32_t seed = UINT32_C(0x718a62e9);
    for (size_t i = 0; i < BUFFER_BYTES; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        buf[i] = (uint8_t)seed;
    }
    fprintf(stderr, "SDK Linux backend, seed=0x718a62e9 samples=%u target_bytes=%u\n",
            SAMPLES, SAMPLE_BYTES);
    puts("path,bytes,offset,calls,baseline_ns,sdk_host_ns,ratio,checksum");
    for (unsigned off = 0; off <= 3; off += 3) {
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
            scenario("vehicle_bitwise", crc_bitwise_reference, vp_crc32, buf, lengths[i], off);
            scenario("video_nibble", crc_nibble_reference, obv1_crc32, buf, lengths[i], off);
        }
    }
    free(buf);
    return 0;
}
