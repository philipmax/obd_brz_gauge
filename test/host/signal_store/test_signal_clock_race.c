#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "app_obd_dsp/signal_store.h"

static int64_t now_us;
static bool inject_writer;
static unsigned depth;

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{ (void)caps; return calloc(count, size); }
int64_t esp_timer_get_time(void) { return now_us; }

/* Simulate another core committing a sample between the caller's attempted
 * lock and successful acquisition, without nesting the critical section. */
void ss_test_enter(portMUX_TYPE *mux)
{
    (void)mux;
    assert(depth == 0);
    if (inject_writer) {
        inject_writer = false;
        now_us += 3000;
        assert(signal_store_set(SIG_OBD_RPM, 3200));
    }
    depth = 1;
}
void ss_test_exit(portMUX_TYPE *mux) { (void)mux; assert(depth == 1); depth = 0; }

static void seed(int64_t now)
{
    now_us = now;
    assert(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_DATA));
    assert(signal_store_set(SIG_OBD_RPM, 1000));
    assert(signal_store_set(SIG_OBD_SPEED, 60));
}

int main(void)
{
    signal_store_init();
    seed(1000000);
    int32_t value = 0;
    inject_writer = true;
    assert(signal_store_get(SIG_OBD_RPM, &value) && value == 3200);

    seed(2000000);
    int slots[] = { SIG_OBD_RPM, SIG_OBD_SPEED };
    int32_t values[2];
    inject_writer = true;
    assert(signal_store_get_many(slots, 1, values) == 1 && values[0] == 3200);
    seed(3000000);
    inject_writer = true;
    assert(signal_store_get_many(slots, 2, values) == 3 && values[0] == 3200 && values[1] == 60);

    seed(4000000);
    double delivered[SIG_SLOT_COUNT] = {0};
    uint64_t mask = (UINT64_C(1) << SIG_OBD_RPM) | (UINT64_C(1) << SIG_OBD_SPEED);
    inject_writer = true;
    assert(signal_store_get_delivered_mask(mask, delivered) == mask && delivered[SIG_OBD_RPM] == 3200);

    /* A delayed writer must timestamp at its actual commit, not before waiting. */
    seed(5000000);
    inject_writer = true;
    assert(signal_store_set(SIG_OBD_RPM, 2500));
    signal_entry_t entry;
    signal_store_entry(SIG_OBD_RPM, &entry);
    assert(entry.ts_ms == 5003 && entry.value == 2500);

    /* The same ordering works when the 32-bit millisecond clock wraps. */
    seed((int64_t)(UINT32_MAX - 1u) * 1000);
    inject_writer = true;
    assert(signal_store_get(SIG_OBD_RPM, &value) && value == 3200);
    assert(depth == 0);
    return 0;
}
