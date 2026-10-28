#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "app_obd_dsp/ota_idle_tracker.h"

/* The actual task is extracted from ota_wifi_server.c at build time. These
 * boundaries control time, lock contention, activity and stop failures. */
#define OTA_IDLE_TIMEOUT_US INT64_C(100)
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_LOGW(tag, ...) ((void)(tag))
static const char *TAG = "test";
static int s_idle_mux;
static ota_idle_tracker_t s_idle;
static int64_t clock_us;
static unsigned locks, stops, delays;
static bool held, busy_once, refresh_on_lock, restart_on_lock, stop_fail_once;

static int64_t esp_timer_get_time(void) { return clock_us; }
static bool lifecycle_try_take(void)
{
    locks++;
    if (busy_once) { busy_once = false; return false; }
    assert(!held); held = true;
    if (refresh_on_lock) {
        refresh_on_lock = false;
        ota_idle_state(&s_idle, OTA_WIFI_STATE_RECEIVING, clock_us);
    }
    if (restart_on_lock) {
        restart_on_lock = false;
        ota_idle_begin(&s_idle, clock_us);
    }
    return true;
}
static void lifecycle_give(void) { assert(held); held = false; }
static bool ota_wifi_server_stop_impl(void)
{
    assert(held); stops++;
    if (stop_fail_once) { stop_fail_once = false; return false; }
    ota_idle_state(&s_idle, OTA_WIFI_STATE_IDLE, clock_us);
    return true;
}
static void vTaskDelay(unsigned ms)
{
    assert(ms == 2000 && ++delays < 5);
    clock_us += 2000;
}
static void vTaskDelete(void *task) { assert(task == NULL && !held); }
#include "ota_timeout_task.inc"

static uint32_t reset(void)
{
    s_idle = (ota_idle_tracker_t){0}; clock_us = 0;
    locks = stops = delays = 0;
    held = busy_once = refresh_on_lock = restart_on_lock = stop_fail_once = false;
    return ota_idle_begin(&s_idle, clock_us);
}

int main(void)
{
    uint32_t first = reset();
    ota_idle_state(&s_idle, OTA_WIFI_STATE_IDLE, 1);
    ota_idle_begin(&s_idle, 2);
    clock_us = 1000;
    ota_timeout_task((void *)(uintptr_t)first);
    assert(stops == 0 && locks == 0);

    first = reset(); clock_us = 101; busy_once = true;
    ota_timeout_task((void *)(uintptr_t)first);
    assert(locks == 2 && delays == 1 && stops == 1 && s_idle.state == OTA_WIFI_STATE_IDLE);

    first = reset(); clock_us = 101; refresh_on_lock = true;
    ota_timeout_task((void *)(uintptr_t)first);
    assert(locks == 2 && delays == 1 && stops == 1); /* only after new inactivity */

    first = reset(); clock_us = 101; restart_on_lock = true;
    ota_timeout_task((void *)(uintptr_t)first);
    assert(locks == 1 && stops == 0 && s_idle.generation != first);

    first = reset(); clock_us = 101; stop_fail_once = true;
    ota_timeout_task((void *)(uintptr_t)first);
    assert(locks == 2 && delays == 1 && stops == 2 && s_idle.state == OTA_WIFI_STATE_IDLE);
    return 0;
}
