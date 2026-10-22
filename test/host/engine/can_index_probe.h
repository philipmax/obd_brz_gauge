#pragma once
/* Rejected host-only CAN index experiment; no firmware uses this code.
 * Session-local CAN rule index. Equal IDs retain original profile order so
 * fallback/promotion decisions made by one rule reach the next as before. */
#include "vp_profile.h"

#define VP_CAN_INDEX_MIN 5u

static inline void vp_can_index_build(const vp_can_rule_t *rules, uint16_t count,
                                      uint8_t order[VP_MAX_CAN_RULES])
{
    for (uint16_t i = 0; i < count; i++) {
        uint16_t j = i;
        while (j && rules[order[j - 1]].can_id > rules[i].can_id) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = (uint8_t)i;
    }
}

/* First matching position in order[], or count when absent. */
static inline uint16_t vp_can_index_first(const vp_can_rule_t *rules,
                                         const uint8_t *order, uint16_t count,
                                         uint32_t id)
{
    uint16_t lo = 0, hi = count;
    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
        if (rules[order[mid]].can_id < id) lo = (uint16_t)(mid + 1u);
        else hi = mid;
    }
    return lo < count && rules[order[lo]].can_id == id ? lo : count;
}

/* Timed implementations live in a separate translation unit without LTO. */
uint64_t visit_linear(const vp_can_rule_t *rules, const uint8_t *order,
                      uint16_t count, uint32_t id);
uint64_t visit_indexed(const vp_can_rule_t *rules, const uint8_t *order,
                       uint16_t count, uint32_t id);
