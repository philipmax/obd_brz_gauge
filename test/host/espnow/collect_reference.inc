/* Frozen original collector, on the same stub inputs and metadata. */
static void master_collect_reference(espnow_snapshot_t *s, const nvs_user_cfg_t *cfg,
                           linktest_sample_t test) {

    espnow_snapshot_init(s);
    s->seq          = ++s_tx_seq;
    s->profile_hash = local_profile_hash();
    // bit0 = data flowing (sys.obd_link == 2); bit1 = linked test in progress; bit2 = rpm_warn_linked_en (slaves mirror it)
    // Read sys.obd_link together with the OBD range so this 10 Hz path enters
    // signal_store only once for both the link flag and the payload values.
    enum { SNAP_LINK_INDEX = SIG_OBD_COUNT };
    enum { SNAP_BATCH_CAP = (SIG_OBD_COUNT + 1) > SIG_VEH_MAX ? (SIG_OBD_COUNT + 1) : SIG_VEH_MAX };
    int slots[SNAP_BATCH_CAP];
    int32_t values[SNAP_BATCH_CAP];
    for (int i = 0; i < SIG_OBD_COUNT; i++) slots[i] = SIG_OBD_FIRST + i;
    slots[SNAP_LINK_INDEX] = SIG_SYS_OBD_LINK;
    uint64_t valid = signal_store_get_many(slots, SIG_OBD_COUNT + 1, values);
    const bool link = (valid & (UINT64_C(1) << SNAP_LINK_INDEX)) && values[SNAP_LINK_INDEX] == SIG_LINK_DATA;
    s->flags = (uint8_t)((link ? ESPNOW_FLAG_ELM : 0)
                       | (test.active ? ESPNOW_FLAG_LINKTEST : 0)
                       | (cfg->rpm_warn_linked_en ? ESPNOW_FLAG_LINKED_EN : 0));
    s->sweep_step = (uint8_t)ui_sweep_get_step();   // broadcast sweep progress for slave sync
    s->intro_step = (uint8_t)ui_intro_get_step();   // broadcast boot-animation progress for slave sync
    memcpy(s->name, MASTER_NAME, sizeof(MASTER_NAME));

    // The first batch also includes sys.obd_link. A profile change between the
    // two batches is harmless: removed slots are reported invalid by the store.
    for (int i = 0; i < SIG_OBD_COUNT; i++) {
        if (valid & (UINT64_C(1) << i)) {
            espnow_snapshot_put(s, sig_registry[slots[i]].num_id, values[i]);
        }
    }

    int n_veh = signal_store_veh_count();
    if (n_veh > SIG_VEH_MAX) n_veh = SIG_VEH_MAX;
    for (int i = 0; i < n_veh; i++) slots[i] = SIG_VEH_BASE + i;
    valid = signal_store_get_many(slots, (size_t)n_veh, values);
    for (int i = 0; i < n_veh; i++) {
        if (valid & (UINT64_C(1) << i)) {
            const uint16_t num_id = (uint16_t)(SIG_VEH_ID_BASE + i);
            espnow_snapshot_put(s, num_id, values[i]);
        }
    }
    // Linked test: slaves follow the master's simulated RPM ramp (the master shows it via the override layer).
    if (test.active) espnow_snapshot_put(s, ESPNOW_ID_RPM, test.rpm);
}
