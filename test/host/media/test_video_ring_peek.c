/* Exercise the SAME inline head reader used by video_player.c. The reference
 * is the old locked reader. Tests also forbid ring-memory access while locked
 * and publish producer records at lock boundaries and on a second thread. */
#include "video_ring_peek.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); \
} } while (0)

static void *guard_page;
static size_t guard_length;
static _Thread_local void (*after_unlock)(void);
static _Thread_local unsigned lock_depth;

void ring_test_enter(portMUX_TYPE *lock)
{
    CHECK(pthread_mutex_lock(&lock->mutex) == 0);
    CHECK(lock_depth++ == 0);
    if (guard_page) CHECK(mprotect(guard_page, guard_length, PROT_NONE) == 0);
}

void ring_test_exit(portMUX_TYPE *lock)
{
    if (guard_page) CHECK(mprotect(guard_page, guard_length, PROT_READ | PROT_WRITE) == 0);
    CHECK(lock_depth-- == 1);
    CHECK(pthread_mutex_unlock(&lock->mutex) == 0);
    void (*hook)(void) = after_unlock;
    after_unlock = NULL; /* allow a hook to publish using the same mutex */
    if (hook) hook();
}

typedef struct {
    portMUX_TYPE lock;
    uint32_t storage[128];
    uint32_t capacity, wpos, rpos, used, produced, consumed;
} test_ring_t;

static uint32_t random_next(uint32_t *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}

/* Production reserve/commit/release protocol, with no lock held during writes.
 * Sizes are chosen so capacity >= 2 * maximum record length, as in the player. */
static bool publish(test_ring_t *ring, uint32_t id, uint32_t size)
{
    uint32_t need = 8u + ((size + 3u) & ~3u);
    taskENTER_CRITICAL(&ring->lock);
    uint32_t pos = ring->wpos;
    uint32_t tail = ring->capacity - pos;
    uint32_t waste = tail < need ? tail : 0;
    bool fits = ring->capacity - ring->used >= waste + need;
    taskEXIT_CRITICAL(&ring->lock);
    if (!fits) return false;
    if (waste >= 8u) {
        uint32_t *wrap = ring->storage + pos / 4u;
        wrap[0] = UINT32_MAX;
        wrap[1] = 0;
    }
    uint32_t at = waste ? 0 : pos;
    uint32_t *header = ring->storage + at / 4u;
    header[0] = id;
    header[1] = size;
    uint8_t *payload = (uint8_t *)header + 8u;
    for (uint32_t i = 0; i < size; i++) payload[i] = (uint8_t)(id + i);
    taskENTER_CRITICAL(&ring->lock);
    ring->used += waste + need;
    ring->wpos = at + need;
    if (ring->wpos == ring->capacity) ring->wpos = 0;
    ring->produced++;
    taskEXIT_CRITICAL(&ring->lock);
    return true;
}

static void release(test_ring_t *ring, uint32_t length)
{
    taskENTER_CRITICAL(&ring->lock);
    CHECK(length <= ring->used);
    ring->used -= length;
    ring->rpos += length;
    if (ring->rpos == ring->capacity) ring->rpos = 0;
    ring->consumed++;
    taskEXIT_CRITICAL(&ring->lock);
}

static bool peek(test_ring_t *ring, uint32_t *size, const uint8_t **data, uint32_t *length)
{
    return vp_ring_peek_committed((const uint8_t *)ring->storage, ring->capacity,
                                 &ring->rpos, &ring->used, &ring->lock, size, data, length);
}

static bool old_peek(test_ring_t *ring, uint32_t *size, const uint8_t **data, uint32_t *length)
{
    while (ring->used) {
        uint32_t tail = ring->capacity - ring->rpos;
        const uint32_t *header = ring->storage + ring->rpos / 4u;
        if (tail < 8u || header[0] == UINT32_MAX) {
            ring->used -= tail;
            ring->rpos = 0;
            continue;
        }
        *size = header[1];
        *data = (const uint8_t *)header + 8u;
        *length = 8u + ((header[1] + 3u) & ~3u);
        return true;
    }
    return false;
}

static void check_payload(const uint8_t *data, uint32_t size, uint32_t id)
{
    CHECK(((const uint32_t *)data)[-2] == id);
    for (uint32_t i = 0; i < size; i++) CHECK(data[i] == (uint8_t)(id + i));
}

static void test_differential(void)
{
    const uint32_t capacities[] = {24, 32, 64, 128, 256, 512};
    uint32_t seed = 0x23154987u;
    for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
        test_ring_t ring = {.lock = portMUX_INITIALIZER_UNLOCKED, .capacity = capacities[c]};
        for (unsigned i = 0; i < 40000; i++) {
            uint32_t choice = random_next(&seed);
            uint32_t max_size = ring.capacity / 2u - 8u;
            if (choice & 3u) (void)publish(&ring, ring.produced, 1u + choice % max_size);
            /* Only clone protocol data, never a live pthread mutex. */
            test_ring_t reference = {.capacity = ring.capacity, .rpos = ring.rpos, .used = ring.used};
            memcpy(reference.storage, ring.storage, sizeof(ring.storage));
            uint32_t size = 0xBADu, expected_size = size, length = 0xBADu, expected_length = length;
            const uint8_t *data = NULL, *expected_data = NULL;
            bool expected = old_peek(&reference, &expected_size, &expected_data, &expected_length);
            bool actual = peek(&ring, &size, &data, &length);
            CHECK(actual == expected);
            CHECK(size == expected_size && length == expected_length);
            CHECK(ring.rpos == reference.rpos && ring.used == reference.used);
            if (actual) {
                CHECK(data - (const uint8_t *)ring.storage ==
                      expected_data - (const uint8_t *)reference.storage);
                check_payload(data, size, ring.consumed);
                if (choice & 4u) release(&ring, length); /* repeated peek keeps the head alive */
            } else CHECK(data == NULL);
            CHECK(ring.used <= ring.capacity);
        }
        CHECK(pthread_mutex_destroy(&ring.lock.mutex) == 0);
    }
}

/* Protect the stand-in PSRAM page on EVERY lock entry. Any ring read while
 * holding the production critical section faults, including wrap headers. */
static void test_no_ring_reads_under_lock(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0);
    guard_length = (size_t)page_size;
    void *page = mmap(NULL, guard_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    const uint32_t capacity = 64;
    uint32_t *words = page;
    /* Test empty, every aligned record position, explicit and implicit wrap. */
    for (unsigned kind = 0; kind < 4; kind++) {
        for (uint32_t pos = 0; pos < capacity; pos += 4) {
            memset(page, 0, guard_length);
            uint32_t rpos = pos, used = kind == 0 ? 0 : capacity - pos;
            if (kind == 1 && capacity - pos < 12u) continue;
            if (kind == 1) { words[pos / 4u] = 42; words[pos / 4u + 1u] = 3; }
            else if (kind >= 2) {
                if (capacity - pos >= 8u) words[pos / 4u] = UINT32_MAX;
                if (kind == 3) {
                    if (pos < 12u) continue;
                    words[0] = 43; words[1] = 4;
                    used += 12u;
                }
            }
            uint32_t size = 77, length = 88;
            const uint8_t *data = NULL;
            guard_page = page;
            bool found = vp_ring_peek_committed(page, capacity, &rpos, &used, &lock, &size, &data, &length);
            guard_page = NULL;
            CHECK(found == (kind == 1 || kind == 3));
            if (found) {
                CHECK(length == 12u);
                CHECK(size == (kind == 1 ? 3u : 4u));
                CHECK(data == (uint8_t *)page + (kind == 1 ? pos : 0) + 8u);
                CHECK(used == (kind == 1 ? capacity - pos : 12u));
            } else CHECK(size == 77 && length == 88 && data == NULL);
        }
    }
    CHECK(pthread_mutex_destroy(&lock.mutex) == 0);
    CHECK(munmap(page, guard_length) == 0);
}

static test_ring_t *injected_ring;
static void inject_publish(void)
{
    CHECK(publish(injected_ring, injected_ring->produced, 4));
}

static void test_commit_after_snapshot(void)
{
    test_ring_t ring = {.lock = portMUX_INITIALIZER_UNLOCKED, .capacity = 64};
    /* Empty at nonzero rpos. A commit after the empty snapshot can defer work
     * until the next peek, but MUST NOT expose an uncommitted head. */
    ring.rpos = ring.wpos = 60;
    injected_ring = &ring;
    after_unlock = inject_publish;
    uint32_t size = 0, length = 0;
    const uint8_t *data = NULL;
    CHECK(!peek(&ring, &size, &data, &length));
    CHECK(ring.used == 16); /* implicit tail 4 + record 12 */
    CHECK(peek(&ring, &size, &data, &length));
    check_payload(data, size, 0);
    release(&ring, length);

    /* Committed explicit wrap marker + head at 0; publish a second head after
     * the cursor snapshot. Padding subtraction must retain the new commit. */
    ring.rpos = 52; ring.wpos = 0; ring.used = 12;
    ring.storage[13] = UINT32_MAX;
    CHECK(publish(&ring, ring.produced, 4));
    after_unlock = inject_publish;
    CHECK(peek(&ring, &size, &data, &length));
    CHECK(ring.rpos == 0 && ring.used == 24);
    check_payload(data, size, 1);
    release(&ring, length);
    CHECK(peek(&ring, &size, &data, &length));
    check_payload(data, size, 2);
    release(&ring, length);
    CHECK(ring.used == 0);
    CHECK(pthread_mutex_destroy(&ring.lock.mutex) == 0);
    injected_ring = NULL;
}

#define STRESS_RECORDS 200000u
static void *producer_thread(void *arg)
{
    test_ring_t *ring = arg;
    uint32_t seed = 0x7DD34941u;
    for (uint32_t id = 0; id < STRESS_RECORDS; id++) {
        uint32_t size = 1u + random_next(&seed) % 80u;
        while (!publish(ring, id, size)) sched_yield();
        if (id % 97u == 0) sched_yield();
    }
    return NULL;
}

static void test_concurrent_producer(void)
{
    test_ring_t ring = {.lock = portMUX_INITIALIZER_UNLOCKED, .capacity = 512};
    pthread_t producer;
    CHECK(pthread_create(&producer, NULL, producer_thread, &ring) == 0);
    for (uint32_t id = 0; id < STRESS_RECORDS; id++) {
        uint32_t size, length;
        const uint8_t *data;
        while (!peek(&ring, &size, &data, &length)) sched_yield();
        check_payload(data, size, id);
        /* Producer must not overwrite this head while consumer is delayed. */
        if (id % 53u == 0) sched_yield();
        check_payload(data, size, id);
        release(&ring, length);
    }
    CHECK(pthread_join(producer, NULL) == 0);
    CHECK(ring.produced == STRESS_RECORDS && ring.consumed == STRESS_RECORDS && ring.used == 0);
    CHECK(pthread_mutex_destroy(&ring.lock.mutex) == 0);
}

int main(void)
{
    test_differential();
    test_no_ring_reads_under_lock();
    test_commit_after_snapshot();
    test_concurrent_producer();
    puts("video ring: old-reader equivalence, lock-page guard, injected commits, 200000 concurrent records OK");
    return 0;
}
