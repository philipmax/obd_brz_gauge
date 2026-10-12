#pragma once
/*
 * vp_scheduler — generic request/decode engine for vehicle profiles
 *
 * Replaces the hardcoded 12-slot switch and parallel oil-temp mechanisms in
 * elm327_ble_client.c with a data-driven scheduler that executes requests
 * from a loaded vehicle profile.
 *
 * Architecture:
 *   - Single-threaded state machine (per AGENTS.md constraint)
 *   - Reads vp_profile_t and generates AT commands + OBD requests
 *   - Parses responses using profile's RPN formulas
 *   - Updates signal_store with decoded values
 *   - Handles request failures, source fallback, and rate limiting
 *
 * Integration point: called from elm327_ble_client.c poll task, replacing
 * the existing do_obd_poll() logic.
 */

#include <stdint.h>
#include <stdbool.h>
#include "vehicle/vp_profile.h"
#include "app_obd_dsp/signal_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scheduler state */
typedef enum {
    VP_SCHED_STATE_IDLE = 0,
    VP_SCHED_STATE_INIT,          /* Sending AT init commands */
    VP_SCHED_STATE_POLL_WAIT,     /* Waiting for request window */
    VP_SCHED_STATE_SEND,          /* Sending request */
    VP_SCHED_STATE_RECV,          /* Waiting for response */
    VP_SCHED_STATE_DECODE,        /* Decoding response */
    VP_SCHED_STATE_CAN_MONITOR,   /* ATMA monitoring (if enabled) */
} vp_sched_state_t;

/* Per-signal runtime state */
typedef struct {
    uint16_t src_idx;             /* Current source index within signal */
    uint8_t  fail_count;          /* Consecutive failures of current source */
    uint8_t  slow_counter;        /* For VP_RATE_SLOW signals */
    int64_t  last_poll_us;        /* Last poll timestamp */
    float    last_value;          /* For filtering */
    bool     has_value;
} vp_sig_state_t;

/* Per-request runtime state */
typedef struct {
    uint8_t  fail_count;          /* Consecutive failures */
    int64_t  last_send_us;        /* Last transmission */
} vp_req_state_t;

/* CAN monitor buffer */
#define VP_CAN_FRAME_MAX 32
typedef struct {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  len;
} vp_can_frame_t;

/* Main scheduler context */
typedef struct {
    const vp_profile_t *profile;  /* Loaded profile */

    vp_sched_state_t state;
    uint8_t round;                /* Current poll round */
    uint16_t sig_idx;             /* Current signal being polled */
    uint16_t req_idx;             /* Current request being sent */

    /* Runtime state arrays (allocated in PSRAM) */
    vp_sig_state_t *sig_states;
    vp_req_state_t *req_states;
    uint16_t n_sigs;
    uint16_t n_reqs;

    /* Response buffer */
    char rx_buf[512];
    uint16_t rx_len;
    int64_t send_time_us;

    /* CAN monitor */
    vp_can_frame_t can_frames[VP_CAN_FRAME_MAX];
    uint8_t can_frame_count;
    bool can_monitor_active;

    /* Bus state */
    uint16_t current_hdr;
    uint16_t current_rx;
    bool caf0_active;

    /* Statistics */
    uint32_t total_polls;
    uint32_t total_successes;
    uint32_t total_failures;
} vp_scheduler_t;

/**
 * Initialize scheduler with a vehicle profile.
 * Allocates runtime state in PSRAM.
 * Returns ESP_OK on success.
 */
esp_err_t vp_scheduler_init(vp_scheduler_t *sched, const vp_profile_t *profile);

/**
 * Free scheduler resources.
 */
void vp_scheduler_deinit(vp_scheduler_t *sched);

/**
 * Main scheduler tick, called from ELM327 poll task.
 * Returns true if scheduler wants to send a command (call vp_scheduler_get_next_cmd).
 */
bool vp_scheduler_tick(vp_scheduler_t *sched);

/**
 * Get next command to send to ELM327.
 * Returns command string (no CR), or NULL if none.
 */
const char *vp_scheduler_get_next_cmd(vp_scheduler_t *sched);

/**
 * Feed response data to scheduler.
 * Called when ELM327 notify arrives.
 */
void vp_scheduler_feed_response(vp_scheduler_t *sched, const char *data, size_t len);

/**
 * Notify scheduler of CAN frame (from ATMA monitoring).
 */
void vp_scheduler_feed_can_frame(vp_scheduler_t *sched, uint32_t id,
                                  const uint8_t *data, uint8_t len);

/**
 * Force scheduler to specific state (for testing/recovery).
 */
void vp_scheduler_reset(vp_scheduler_t *sched);

/**
 * Get current statistics.
 */
void vp_scheduler_get_stats(const vp_scheduler_t *sched, uint32_t *polls,
                            uint32_t *successes, uint32_t *failures);

#ifdef __cplusplus
}
#endif
