/* Frozen old lookup; uses the same store state and platform primitives. */
int lookup_reference(uint16_t num_id)
{
    for (int i = 0; i < SIG_STD_COUNT; i++) {
        if (sig_registry[i].num_id == num_id) return i;
    }
    if (!s_st || num_id < SIG_VEH_ID_BASE || num_id >= SIG_VEH_ID_BASE + SIG_VEH_MAX) {
        return SIG_SLOT_NONE;
    }
    const int offset = num_id - SIG_VEH_ID_BASE;
    int slot = SIG_SLOT_NONE;
    portENTER_CRITICAL(&s_mux);
    if (offset < s_st->n_veh) slot = SIG_VEH_BASE + offset;
    portEXIT_CRITICAL(&s_mux);
    return slot;
}

