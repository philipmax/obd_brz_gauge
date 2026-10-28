#include <assert.h>
#include <stdint.h>
#include "app_obd_dsp/ota_idle_tracker.h"

int main(void)
{
    ota_idle_tracker_t t = {0};
    assert(!ota_idle_current(&t, 0));
    uint32_t first = ota_idle_begin(&t, INT64_C(0x1fffffffe));
    assert(first != 0 && ota_idle_current(&t, first));
    ota_idle_state(&t, OTA_WIFI_STATE_READY, INT64_C(0x200000000));
    assert(!ota_idle_expired(&t, first, INT64_C(0x200000064), 100));
    assert(ota_idle_expired(&t, first, INT64_C(0x200000065), 100));
    ota_idle_state(&t, OTA_WIFI_STATE_RECEIVING, INT64_C(0x200000050));
    ota_idle_state(&t, OTA_WIFI_STATE_DONE, INT64_C(0x200000020));
    assert(t.last_activity_us == INT64_C(0x200000050));
    assert(!ota_idle_expired(&t, first, INT64_C(0x200000065), 100));
    ota_idle_state(&t, OTA_WIFI_STATE_IDLE, INT64_C(0x200000060));
    assert(!ota_idle_current(&t, first));
    uint32_t second = ota_idle_begin(&t, INT64_C(0x300000000));
    assert(second != first && ota_idle_current(&t, second));
    assert(!ota_idle_current(&t, first));
    assert(!ota_idle_expired(&t, first, INT64_C(0x300ffffff), 100));
    assert(ota_idle_expired(&t, second, INT64_C(0x300000065), 100));
    assert(!ota_idle_expired(&t, second, INT64_C(0x2ffffffff), 100));
    t.generation = UINT32_MAX;
    assert(ota_idle_begin(&t, 0) == 1);
    return 0;
}
