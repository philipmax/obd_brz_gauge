#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_obd_dsp/signal_store.h"

int lookup_reference(uint16_t num_id);
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { if (failures++ < 10) \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{ (void)caps; return calloc(n, size); }
int64_t esp_timer_get_time(void) { return 0; }

static int standard_reference(uint16_t id)
{
    for (int i = 0; i < SIG_STD_COUNT; i++)
        if (sig_registry[i].num_id == id) return i;
    return SIG_SLOT_NONE;
}

static void sweep(int n_veh)
{
    for (unsigned id = 0; id < 65536; id++) {
        int expected = standard_reference((uint16_t)id);
        if (expected < 0 && id >= SIG_VEH_ID_BASE && id < SIG_VEH_ID_BASE + (unsigned)n_veh)
            expected = SIG_VEH_BASE + (int)(id - SIG_VEH_ID_BASE);
        CHECK(sig_registry_slot_by_id((uint16_t)id) == standard_reference((uint16_t)id));
        CHECK(signal_store_slot_by_id((uint16_t)id) == expected);
        CHECK(lookup_reference((uint16_t)id) == expected);
    }
}

int main(void)
{
    sweep(0);  /* Static lookup works before store allocation. */
    signal_store_init();
    sweep(0);
    for (int count = 1; count <= SIG_VEH_MAX; count++) {
        char key[32]; snprintf(key, sizeof(key), "veh.lookup%d", count);
        CHECK(signal_store_veh_register(key, -1000, 1000, 5000) == SIG_VEH_BASE + count - 1);
        CHECK(signal_store_veh_count() == count);
        sweep(count);
    }
    CHECK(signal_store_veh_register("veh.full", 0, 100, 0) == SIG_SLOT_NONE);
    signal_store_veh_clear();
    CHECK(signal_store_veh_count() == 0);
    sweep(0);
    /* Clear/re-register reuses the existing dynamic ID sequence. */
    CHECK(signal_store_veh_register("veh.again", -100, 100, 0) == SIG_VEH_BASE);
    sweep(1);
    printf("signal_lookup: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
