/* Real private collector and frozen baseline; dead stripping excludes unused SDK paths. */
#include "collect_probe.h"
#include "../../../main/bsp_obd_dsp/espnow_link.c"
#include "collect_reference.inc"

static const collect_input_t *probe_input;
uint64_t signal_store_get_many(const int *slots, size_t count, int32_t *values)
{
    uint64_t valid = 0;
    for (size_t i = 0; i < count; i++) {
        const int slot = slots[i];
        values[i] = probe_input->values[slot];
        if (slot >= SIG_VEH_BASE && slot - SIG_VEH_BASE >= probe_input->present_vehicle_count) continue;
        if (probe_input->valid_slots & (UINT64_C(1) << slot)) valid |= UINT64_C(1) << i;
    }
    return valid;
}
int signal_store_veh_count(void) { return probe_input->reported_vehicle_count; }
int ui_sweep_get_step(void) { return probe_input->sweep; }
int ui_intro_get_step(void) { return probe_input->intro; }

void collect_probe_current(const collect_input_t *input, espnow_snapshot_t *out)
{
    probe_input = input; s_tx_seq = input->seq_before; s_local_profile_hash = input->hash;
    master_collect(out, &input->cfg, (linktest_sample_t){ input->test_active, input->test_rpm });
}
void collect_probe_reference(const collect_input_t *input, espnow_snapshot_t *out)
{
    probe_input = input; s_tx_seq = input->seq_before; s_local_profile_hash = input->hash;
    master_collect_reference(out, &input->cfg, (linktest_sample_t){ input->test_active, input->test_rpm });
}
