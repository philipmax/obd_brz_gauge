#pragma once
/* Internal SPSC ring head reader. The producer publishes whole records under
 * lock; the sole consumer owns rpos and releases each record after use. ring
 * and capacity stay fixed until both tasks have dropped their session refs.
 * Keep PSRAM header loads OUTSIDE the critical section: a published head (and
 * its wrap marker) cannot be overwritten until this consumer releases it.
 */
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

static inline bool vp_ring_peek_committed(const uint8_t *ring, uint32_t capacity,
                                         uint32_t *rpos, uint32_t *used,
                                         portMUX_TYPE *lock, uint32_t *size,
                                         const uint8_t **data, uint32_t *rec_len)
{
    for (;;) {
        taskENTER_CRITICAL(lock);
        uint32_t pos = *rpos;
        bool available = *used != 0;
        taskEXIT_CRITICAL(lock);
        if (!available) return false;

        uint32_t tail = capacity - pos;
        if (tail >= 8u) {
            const uint32_t *header = (const uint32_t *)(ring + pos);
            if (header[0] != UINT32_MAX) {
                uint32_t bytes = header[1];
                *size = bytes;
                *data = ring + pos + 8u;
                *rec_len = 8u + ((bytes + 3u) & ~3u);
                return true;
            }
        }
        /* Only the producer can have changed used since the snapshot: subtract
         * padding from its CURRENT value, not the snapshot, preserving commits
         * that arrived while the PSRAM header was being read. */
        taskENTER_CRITICAL(lock);
        *used -= tail;
        *rpos = 0;
        taskEXIT_CRITICAL(lock);
    }
}
