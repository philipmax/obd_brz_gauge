#include "signal_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"
#include <string.h>

static signal_entry_t s_signals[SIGNAL_TOTAL];
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t        s_veh_next = 0;  /* next free veh.* slot offset within VEH range */

void signal_store_init(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_signals, 0, sizeof(s_signals));
    /* all entries start invalid; value/ts/seq are all zero */
    portEXIT_CRITICAL(&s_mux);
    s_veh_next = 0;
}

void signal_store_set(uint8_t idx, int32_t val)
{
    if (idx >= SIGNAL_TOTAL) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_mux);
    s_signals[idx].value = val;
    s_signals[idx].valid = true;
    s_signals[idx].ts_ms = now_ms;
    s_signals[idx].seq++;
    portEXIT_CRITICAL(&s_mux);
}

void signal_store_invalidate(uint8_t idx)
{
    if (idx >= SIGNAL_TOTAL) return;
    portENTER_CRITICAL(&s_mux);
    s_signals[idx].valid = false;
    s_signals[idx].seq++;
    portEXIT_CRITICAL(&s_mux);
}

void signal_store_get(uint8_t idx, signal_entry_t *out)
{
    if (idx >= SIGNAL_TOTAL || !out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_signals[idx];
    portEXIT_CRITICAL(&s_mux);
}

int32_t signal_store_value(uint8_t idx)
{
    if (idx >= SIGNAL_TOTAL) return 0;
    int32_t v;
    portENTER_CRITICAL(&s_mux);
    v = s_signals[idx].valid ? s_signals[idx].value : 0;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

bool signal_store_is_valid(uint8_t idx)
{
    if (idx >= SIGNAL_TOTAL) return false;
    bool v;
    portENTER_CRITICAL(&s_mux);
    v = s_signals[idx].valid;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

uint64_t signal_store_valid_mask(void)
{
    uint64_t mask = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SIGNAL_TOTAL; i++) {
        if (s_signals[i].valid) mask |= (UINT64_C(1) << i);
    }
    portEXIT_CRITICAL(&s_mux);
    return mask;
}

void signal_store_reset_all(void)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SIGNAL_TOTAL; i++) {
        s_signals[i].valid = false;
        s_signals[i].seq++;
    }
    portEXIT_CRITICAL(&s_mux);
}

int signal_store_veh_alloc(void)
{
    if (s_veh_next >= SIG_VEH_COUNT) return -1;
    int idx = SIG_VEH_BASE + s_veh_next;
    s_veh_next++;
    return idx;
}
