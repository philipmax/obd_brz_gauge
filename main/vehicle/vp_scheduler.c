/*
 * vp_scheduler.c — generic request/decode engine
 */

#include "vp_scheduler.h"
#include "vp_rpn.h"
#include "app_obd_dsp/signal_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "vp_sched";

/* Rate limits (microseconds) */
#define VP_RATE_FAST_US    (100 * 1000)    /* 100ms */
#define VP_RATE_MEDIUM_US  (500 * 1000)    /* 500ms */
#define VP_RATE_SLOW_US    (2000 * 1000)   /* 2s */
#define VP_SLOW_ROUNDS     10              /* Poll slow signals every N rounds */

/* Failure thresholds */
#define VP_SRC_FAIL_MAX    3               /* Source failures before fallback */
#define VP_REQ_FAIL_MAX    5               /* Request failures before skip */

/* Response timeout */
#define VP_RESPONSE_TIMEOUT_US (1000 * 1000) /* 1s */

/* Command buffer */
static char s_cmd_buf[128];

/* Helper: parse hex byte */
static inline uint8_t parse_hex_byte(const char *s) {
    uint8_t v = 0;
    for (int i = 0; i < 2; i++) {
        v <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    }
    return v;
}

/* Helper: parse OBD response "41 0C 1A F8" → [0x41, 0x0C, 0x1A, 0xF8] */
static int parse_obd_response(const char *resp, uint8_t *out, size_t max_len) {
    int count = 0;
    const char *p = resp;

    while (*p && count < max_len) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        /* Check if we have two hex digits */
        if (!((p[0] >= '0' && p[0] <= '9') || (p[0] >= 'A' && p[0] <= 'F') || (p[0] >= 'a' && p[0] <= 'f')))
            break;
        if (!((p[1] >= '0' && p[1] <= '9') || (p[1] >= 'A' && p[1] <= 'F') || (p[1] >= 'a' && p[1] <= 'f')))
            break;

        out[count++] = parse_hex_byte(p);
        p += 2;
    }

    return count;
}

/* Helper: parse CAN frame "7E8 03 41 0C 1A" */
static bool parse_can_frame(const char *line, vp_can_frame_t *frame) {
    const char *p = line;

    /* Parse CAN ID */
    uint32_t id = 0;
    int id_digits = 0;
    while (*p && id_digits < 8) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            id = (id << 4) | (c - '0');
            id_digits++;
        } else if (c >= 'A' && c <= 'F') {
            id = (id << 4) | (c - 'A' + 10);
            id_digits++;
        } else if (c >= 'a' && c <= 'f') {
            id = (id << 4) | (c - 'a' + 10);
            id_digits++;
        } else if (c == ' ') {
            break;
        } else {
            return false;
        }
        p++;
    }

    if (id_digits < 3) return false;
    frame->id = id;

    /* Skip whitespace */
    while (*p == ' ') p++;

    /* Parse data bytes */
    frame->len = 0;
    while (*p && frame->len < 8) {
        if (*p == ' ') { p++; continue; }
        if (!((p[0] >= '0' && p[0] <= '9') || (p[0] >= 'A' && p[0] <= 'F') || (p[0] >= 'a' && p[0] <= 'f')))
            break;
        if (!((p[1] >= '0' && p[1] <= '9') || (p[1] >= 'A' && p[1] <= 'F') || (p[1] >= 'a' && p[1] <= 'f')))
            break;

        frame->data[frame->len++] = parse_hex_byte(p);
        p += 2;
    }

    return frame->len > 0;
}

/* Helper: check if signal should be polled this round */
static bool should_poll_signal(vp_scheduler_t *sched, uint16_t sig_idx) {
    const vp_signal_t *sig = &sched->profile->signals[sig_idx];
    vp_sig_state_t *state = &sched->sig_states[sig_idx];

    /* Check rate */
    if (sig->rate == VP_RATE_SLOW) {
        if (state->slow_counter++ < VP_SLOW_ROUNDS) {
            return false;
        }
        state->slow_counter = 0;
    }

    /* Check time since last poll */
    int64_t now = esp_timer_get_time();
    int64_t min_interval = VP_RATE_MEDIUM_US;

    switch (sig->rate) {
        case VP_RATE_FAST:   min_interval = VP_RATE_FAST_US; break;
        case VP_RATE_MEDIUM: min_interval = VP_RATE_MEDIUM_US; break;
        case VP_RATE_SLOW:   min_interval = VP_RATE_SLOW_US; break;
    }

    if ((now - state->last_poll_us) < min_interval) {
        return false;
    }

    return true;
}

/* Helper: find next signal to poll */
static bool find_next_signal(vp_scheduler_t *sched) {
    for (uint16_t i = 0; i < sched->n_sigs; i++) {
        uint16_t idx = (sched->sig_idx + i) % sched->n_sigs;
        if (should_poll_signal(sched, idx)) {
            sched->sig_idx = idx;
            return true;
        }
    }
    return false;
}

/* Helper: get request for current signal/source */
static const vp_request_t *get_current_request(vp_scheduler_t *sched) {
    const vp_signal_t *sig = &sched->profile->signals[sched->sig_idx];
    vp_sig_state_t *state = &sched->sig_states[sched->sig_idx];

    if (state->src_idx >= sig->n_sources) {
        return NULL;
    }

    uint16_t req_idx = sig->sources[state->src_idx].request_idx;
    if (req_idx >= sched->n_reqs) {
        return NULL;
    }

    sched->req_idx = req_idx;
    return &sched->profile->requests[req_idx];
}

/* Helper: format AT command */
static void format_at_command(char *buf, size_t size, const vp_request_t *req) {
    if (req->service == 0xFF) {
        /* Raw AT command */
        snprintf(buf, size, "%s", req->at_cmd);
    } else {
        /* OBD request */
        if (req->tx_len == 0) {
            snprintf(buf, size, "%02X%02X", req->service, req->pid);
        } else {
            int n = snprintf(buf, size, "%02X%02X", req->service, req->pid);
            for (uint8_t i = 0; i < req->tx_len && n < size - 3; i++) {
                n += snprintf(buf + n, size - n, "%02X", req->tx_data[i]);
            }
        }
    }
}

/* Helper: apply bus configuration for request */
static const char *apply_bus_config(vp_scheduler_t *sched, const vp_request_t *req) {
    bool need_hdr = (req->hdr != 0);
    bool need_rx = (req->rx != 0);
    bool need_caf0 = (req->rx != 0);

    /* Set header if needed */
    if (need_hdr && sched->current_hdr != req->hdr) {
        snprintf(s_cmd_buf, sizeof(s_cmd_buf), "ATSH%04X", req->hdr);
        sched->current_hdr = req->hdr;
        return s_cmd_buf;
    }

    /* Set RX filter if needed */
    if (need_rx && sched->current_rx != req->rx) {
        snprintf(s_cmd_buf, sizeof(s_cmd_buf), "ATCRA%03X", req->rx);
        sched->current_rx = req->rx;
        return s_cmd_buf;
    }

    /* Enable CAF0 if needed */
    if (need_caf0 && !sched->caf0_active) {
        snprintf(s_cmd_buf, sizeof(s_cmd_buf), "ATCAF0");
        sched->caf0_active = true;
        return s_cmd_buf;
    }

    return NULL;
}

/* Helper: decode response using RPN formula */
static bool decode_response(vp_scheduler_t *sched, const uint8_t *data, size_t len, float *out) {
    const vp_signal_t *sig = &sched->profile->signals[sched->sig_idx];
    vp_sig_state_t *state = &sched->sig_states[sched->sig_idx];

    const vp_source_t *src = &sig->sources[state->src_idx];
    if (src->formula_len == 0) {
        return false;
    }

    const uint8_t *formula = sched->profile->rpn_bytecode + src->formula_offset;

    return vp_rpn_eval(formula, src->formula_len, data, len, out);
}

/* Helper: update signal store */
static void update_signal_store(vp_scheduler_t *sched, float value) {
    const vp_signal_t *sig = &sched->profile->signals[sched->sig_idx];
    vp_sig_state_t *state = &sched->sig_states[sched->sig_idx];

    /* Store value */
    signal_store_set(sig->signal_id, value);

    /* Update state */
    state->has_value = true;
    state->last_value = value;
    state->last_poll_us = esp_timer_get_time();
    state->fail_count = 0;
}

/* Helper: handle request failure */
static void handle_request_failure(vp_scheduler_t *sched) {
    vp_sig_state_t *sig_state = &sched->sig_states[sched->sig_idx];
    vp_req_state_t *req_state = &sched->req_states[sched->req_idx];

    sig_state->fail_count++;
    req_state->fail_count++;

    /* Try next source if current source failed too many times */
    if (sig_state->fail_count >= VP_SRC_FAIL_MAX) {
        const vp_signal_t *sig = &sched->profile->signals[sched->sig_idx];
        sig_state->src_idx++;
        sig_state->fail_count = 0;

        if (sig_state->src_idx >= sig->n_sources) {
            /* All sources exhausted, mark signal invalid */
            signal_store_invalidate(sig->signal_id);
            sig_state->src_idx = 0;
            ESP_LOGW(TAG, "Signal %u: all sources failed", sig->signal_id);
        }
    }
}

esp_err_t vp_scheduler_init(vp_scheduler_t *sched, const vp_profile_t *profile) {
    if (!sched || !profile) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(sched, 0, sizeof(*sched));
    sched->profile = profile;
    sched->n_sigs = profile->n_signals;
    sched->n_reqs = profile->n_requests;

    /* Allocate runtime state in PSRAM */
    sched->sig_states = heap_caps_calloc(sched->n_sigs, sizeof(vp_sig_state_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    sched->req_states = heap_caps_calloc(sched->n_reqs, sizeof(vp_req_state_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!sched->sig_states || !sched->req_states) {
        vp_scheduler_deinit(sched);
        return ESP_ERR_NO_MEM;
    }

    sched->state = VP_SCHED_STATE_INIT;

    ESP_LOGI(TAG, "Scheduler init: %u signals, %u requests", sched->n_sigs, sched->n_reqs);
    return ESP_OK;
}

void vp_scheduler_deinit(vp_scheduler_t *sched) {
    if (!sched) return;

    if (sched->sig_states) {
        heap_caps_free(sched->sig_states);
        sched->sig_states = NULL;
    }

    if (sched->req_states) {
        heap_caps_free(sched->req_states);
        sched->req_states = NULL;
    }

    memset(sched, 0, sizeof(*sched));
}

bool vp_scheduler_tick(vp_scheduler_t *sched) {
    if (!sched || !sched->profile) {
        return false;
    }

    int64_t now = esp_timer_get_time();

    switch (sched->state) {
        case VP_SCHED_STATE_IDLE:
            sched->state = VP_SCHED_STATE_INIT;
            return false;

        case VP_SCHED_STATE_INIT:
            /* Initialization done by get_next_cmd */
            return true;

        case VP_SCHED_STATE_POLL_WAIT:
            /* Find next signal to poll */
            if (find_next_signal(sched)) {
                sched->state = VP_SCHED_STATE_SEND;
                return true;
            }

            /* No signals ready, increment round */
            sched->round++;
            sched->sig_idx = 0;
            return false;

        case VP_SCHED_STATE_SEND:
            /* Command will be sent */
            return true;

        case VP_SCHED_STATE_RECV:
            /* Check timeout */
            if ((now - sched->send_time_us) > VP_RESPONSE_TIMEOUT_US) {
                ESP_LOGW(TAG, "Response timeout");
                handle_request_failure(sched);
                sched->state = VP_SCHED_STATE_POLL_WAIT;
            }
            return false;

        case VP_SCHED_STATE_DECODE:
            /* Decode will happen in feed_response */
            sched->state = VP_SCHED_STATE_POLL_WAIT;
            return false;

        case VP_SCHED_STATE_CAN_MONITOR:
            /* CAN monitoring active */
            return false;

        default:
            sched->state = VP_SCHED_STATE_IDLE;
            return false;
    }
}

const char *vp_scheduler_get_next_cmd(vp_scheduler_t *sched) {
    if (!sched || !sched->profile) {
        return NULL;
    }

    switch (sched->state) {
        case VP_SCHED_STATE_INIT: {
            /* Send profile's init AT commands */
            static uint8_t init_idx = 0;
            if (init_idx < sched->profile->n_init_cmds) {
                const char *cmd = sched->profile->init_cmds[init_idx];
                init_idx++;
                return cmd;
            }

            /* Init done */
            init_idx = 0;
            sched->state = VP_SCHED_STATE_POLL_WAIT;
            ESP_LOGI(TAG, "Init complete");
            return NULL;
        }

        case VP_SCHED_STATE_SEND: {
            /* Get current request */
            const vp_request_t *req = get_current_request(sched);
            if (!req) {
                sched->state = VP_SCHED_STATE_POLL_WAIT;
                return NULL;
            }

            /* Check if we need bus config first */
            const char *bus_cmd = apply_bus_config(sched, req);
            if (bus_cmd) {
                return bus_cmd;
            }

            /* Format request command */
            format_at_command(s_cmd_buf, sizeof(s_cmd_buf), req);

            /* Update state */
            sched->send_time_us = esp_timer_get_time();
            sched->rx_len = 0;
            sched->state = VP_SCHED_STATE_RECV;
            sched->total_polls++;

            vp_req_state_t *req_state = &sched->req_states[sched->req_idx];
            req_state->last_send_us = sched->send_time_us;

            return s_cmd_buf;
        }

        default:
            return NULL;
    }
}

void vp_scheduler_feed_response(vp_scheduler_t *sched, const char *data, size_t len) {
    if (!sched || sched->state != VP_SCHED_STATE_RECV) {
        return;
    }

    /* Accumulate response */
    if (sched->rx_len + len < sizeof(sched->rx_buf)) {
        memcpy(sched->rx_buf + sched->rx_len, data, len);
        sched->rx_len += len;
        sched->rx_buf[sched->rx_len] = '\0';
    }

    /* Check for completion markers */
    bool complete = false;
    bool error = false;

    if (strstr(sched->rx_buf, ">") || strstr(sched->rx_buf, "\r>")) {
        complete = true;
    }
    if (strstr(sched->rx_buf, "NO DATA") || strstr(sched->rx_buf, "ERROR") ||
        strstr(sched->rx_buf, "?") || strstr(sched->rx_buf, "CAN ERROR")) {
        complete = true;
        error = true;
    }

    if (!complete) {
        return;
    }

    /* Response complete */
    sched->state = VP_SCHED_STATE_DECODE;

    if (error) {
        ESP_LOGD(TAG, "Request error: %s", sched->rx_buf);
        handle_request_failure(sched);
        sched->total_failures++;
        return;
    }

    /* Parse response */
    uint8_t parsed[64];
    int parsed_len = parse_obd_response(sched->rx_buf, parsed, sizeof(parsed));

    if (parsed_len <= 0) {
        ESP_LOGD(TAG, "Parse failed: %s", sched->rx_buf);
        handle_request_failure(sched);
        sched->total_failures++;
        return;
    }

    /* Decode using RPN */
    float value;
    if (!decode_response(sched, parsed, parsed_len, &value)) {
        ESP_LOGD(TAG, "Decode failed");
        handle_request_failure(sched);
        sched->total_failures++;
        return;
    }

    /* Success */
    update_signal_store(sched, value);
    sched->total_successes++;

    ESP_LOGD(TAG, "Signal %u = %.2f",
             sched->profile->signals[sched->sig_idx].signal_id, value);
}

void vp_scheduler_feed_can_frame(vp_scheduler_t *sched, uint32_t id,
                                  const uint8_t *data, uint8_t len) {
    if (!sched || !sched->can_monitor_active) {
        return;
    }

    if (sched->can_frame_count >= VP_CAN_FRAME_MAX) {
        return;
    }

    vp_can_frame_t *frame = &sched->can_frames[sched->can_frame_count++];
    frame->id = id;
    frame->len = len < 8 ? len : 8;
    memcpy(frame->data, data, frame->len);
}

void vp_scheduler_reset(vp_scheduler_t *sched) {
    if (!sched) return;

    sched->state = VP_SCHED_STATE_INIT;
    sched->round = 0;
    sched->sig_idx = 0;
    sched->req_idx = 0;
    sched->rx_len = 0;

    /* Reset runtime state */
    if (sched->sig_states) {
        memset(sched->sig_states, 0, sched->n_sigs * sizeof(vp_sig_state_t));
    }
    if (sched->req_states) {
        memset(sched->req_states, 0, sched->n_reqs * sizeof(vp_req_state_t));
    }

    /* Reset bus state */
    sched->current_hdr = 0;
    sched->current_rx = 0;
    sched->caf0_active = false;
}

void vp_scheduler_get_stats(const vp_scheduler_t *sched, uint32_t *polls,
                            uint32_t *successes, uint32_t *failures) {
    if (!sched) return;

    if (polls) *polls = sched->total_polls;
    if (successes) *successes = sched->total_successes;
    if (failures) *failures = sched->total_failures;
}
