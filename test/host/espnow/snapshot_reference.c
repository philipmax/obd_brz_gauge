/* Frozen original insertion and mask helpers, independent of the new append path. */
#include "espnow_v6.h"
#include <limits.h>

#define MASK_BITS 32

static bool snapshot_reference_id_is_sys(uint16_t id)
{
    return id >= ESPNOW_ID_SYS_FIRST && id <= ESPNOW_ID_SYS_LAST;
}

static bool snapshot_reference_id_mask_bit(uint16_t id, bool *is_veh, uint32_t *bit)
{
    if (id >= ESPNOW_ID_STD_FIRST && id < ESPNOW_ID_STD_FIRST + MASK_BITS) {
        *is_veh = false;
        *bit = 1u << (id - ESPNOW_ID_STD_FIRST);
        return true;
    }
    if (id >= ESPNOW_ID_VEH_FIRST && id < ESPNOW_ID_VEH_FIRST + MASK_BITS) {
        *is_veh = true;
        *bit = 1u << (id - ESPNOW_ID_VEH_FIRST);
        return true;
    }
    return false;
}

bool snapshot_put_reference(espnow_snapshot_t *s, uint16_t num_id, int32_t value)
{
    if (!s || num_id == 0 || snapshot_reference_id_is_sys(num_id)) return false;

    if (num_id == ESPNOW_ID_GEAR) {
        if (value < INT8_MIN || value >= ESPNOW_GEAR_INVALID) return false;
        s->gear = (int8_t)value;
        s->std_valid |= 1u << (ESPNOW_ID_GEAR - ESPNOW_ID_STD_FIRST);
        return true;
    }

    int i = 0;
    while (i < s->n_rec && s->rec[i].num_id != num_id) i++;
    if (i == s->n_rec) {
        if (s->n_rec >= ESPNOW_SNAP_MAX_REC) return false;
        s->n_rec++;
        s->rec[i].num_id = num_id;
    }
    s->rec[i].value = value;

    bool is_veh;
    uint32_t bit;
    if (snapshot_reference_id_mask_bit(num_id, &is_veh, &bit)) {
        if (is_veh) s->veh_valid |= bit;
        else        s->std_valid |= bit;
    }
    return true;
}

