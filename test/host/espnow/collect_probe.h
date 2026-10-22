#pragma once
#include "bsp_obd_dsp/espnow_v6.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "app_obd_dsp/signal_store.h"

typedef struct {
    nvs_user_cfg_t cfg;
    int32_t values[SIG_SLOT_COUNT];
    uint64_t valid_slots;
    int reported_vehicle_count, present_vehicle_count;
    int sweep, intro;
    uint32_t seq_before, hash;
    bool test_active;
    uint16_t test_rpm;
} collect_input_t;
void collect_probe_current(const collect_input_t *input, espnow_snapshot_t *out);
void collect_probe_reference(const collect_input_t *input, espnow_snapshot_t *out);
