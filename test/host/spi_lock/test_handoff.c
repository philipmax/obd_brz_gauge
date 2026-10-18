/* SDK schedule_core/bg_exit_core run verbatim; only platform boundaries stubbed.
 * This checks lock handoff states, not interrupt timing or LCD performance. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>

#define SPI_BUS_LOCK_ISR_ATTR
#define IRAM_ATTR
#define BUS_LOCK_DEBUG_EXECUTE_CHECK(x) assert(x)
#define LOCK_SHIFT 20
#define PENDING_SHIFT 10
#define REQ_SHIFT 0
#define REQ_MASK 0x3ffu
#define BG_MASK 0xfffffu
#define LOCK_MASK 0x3ff00000u
#define DEV_BG_MASK(dev) ((dev)->mask & BG_MASK)
typedef int BaseType_t;
typedef struct spi_bus_lock_dev_t { uint32_t mask; unsigned wakes; } spi_bus_lock_dev_t;
typedef struct {
    atomic_uint_fast32_t status;
    spi_bus_lock_dev_t *acquiring_dev;
    bool acq_dev_bg_active, in_isr;
    atomic_intptr_t dev[10];
    unsigned enables;
} spi_bus_lock_t;
static int mask_get_id(uint32_t mask) { assert(mask); return __builtin_ffs((int)mask) - 1; }
static void bg_enable(spi_bus_lock_t *lock) { lock->enables++; }
static void resume_dev_in_isr(spi_bus_lock_dev_t *dev, BaseType_t *yield) {
    assert(dev); dev->wakes++; *yield = 1;
}
#include LOCK_FUNCTIONS

static void init(spi_bus_lock_t *lock, spi_bus_lock_dev_t dev[10], uint32_t status) {
    *lock = (spi_bus_lock_t){.in_isr = true};
    atomic_store(&lock->status, status);
    for (unsigned i = 0; i < 10; i++) {
        dev[i] = (spi_bus_lock_dev_t){.mask = (1u << i) | (1u << (i+10)) | (1u << (i+20))};
        atomic_store(&lock->dev[i], (intptr_t)&dev[i]);
    }
}
int main(void) {
    unsigned cases = 0;
    spi_bus_lock_t lock;
    spi_bus_lock_dev_t dev[10], *desired;
    BaseType_t yield;
    /* Actual hang: ISR has finished BG, no owner yet, LOCK waiter arrived
     * on the other core after the ISR's previous scheduling decision. */
    for (unsigned i = 0; i < 10; i++) {
        init(&lock, dev, 1u << (i+20)); yield = 0;
        assert(bg_exit_core(&lock, false, &yield));
        assert(!lock.in_isr);
#ifdef BASELINE
        assert(lock.acquiring_dev == NULL && dev[i].wakes == 0 && yield == 0);
#else
        assert(lock.acquiring_dev == &dev[i] && dev[i].wakes == 1 && yield == 1);
#endif
        cases++;
        /* BG still pending: keep ISR working, do not prematurely wake waiter. */
        init(&lock, dev, (1u << (i+20)) | (1u << i)); yield = 0;
        assert(!bg_exit_core(&lock, false, &yield));
        assert(lock.in_isr && !yield && !dev[i].wakes); cases++;
        /* Existing owner is resumed only after its pending work completes. */
        lock.acquiring_dev = &dev[i]; lock.acq_dev_bg_active = true;
        assert(!bg_exit_core(&lock, false, &yield));
        atomic_store(&lock.status, 1u << (i+20));
        assert(bg_exit_core(&lock, false, &yield));
        assert(dev[i].wakes == 1 && yield == 1 && !lock.in_isr); cases++;
        /* WIP always re-enables ISR; it must not release the hardware. */
        init(&lock, dev, 1u << (i+20)); yield = 0;
        assert(bg_exit_core(&lock, true, &yield));
        assert(lock.in_isr && lock.enables == 1 && !yield); cases++;
        for (unsigned j = 0; j < 10; j++) {
            for (unsigned shift = 0; shift <= 10; shift += 10) {
                init(&lock, dev, (1u << (i+20)) | (1u << (j+shift)));
                bool yields = schedule_core(&lock, (uint32_t)atomic_load(&lock.status), &desired);
#ifdef BASELINE
                assert(desired == &dev[i] && lock.acquiring_dev == &dev[i]);
                assert(yields == (i != j));
#else
                assert(!yields && desired == &dev[j]);
                assert(lock.acquiring_dev == (i == j ? &dev[i] : NULL));
                assert(lock.acq_dev_bg_active == (i == j));
#endif
                cases++;
            }
        }
    }
    init(&lock, dev, 0); yield = 0;
    assert(bg_exit_core(&lock, false, &yield) && !lock.in_isr && !yield);
    assert(schedule_core(&lock, 0, &desired) && desired == NULL);
    printf("%u handoff states verified (%s)\n", cases+2,
#ifdef BASELINE
        "stock SDK missed wake reproduced"
#else
        "patched SDK wakes waiter and preserves active BG"
#endif
    );
    return 0;
}
