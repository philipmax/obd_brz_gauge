/* Independent benchmark translation unit; never linked into firmware. */
#include "can_index_probe.h"

uint64_t visit_linear(const vp_can_rule_t *rules, const uint8_t *order,
                             uint16_t count, uint32_t id)
{
    uint64_t result = 0;
    (void)order;
    for (uint16_t i = 0; i < count; i++) {
        if (rules[i].can_id == id) result = result * 17u + rules[i].src + 1u;
    }
    return result;
}
uint64_t visit_indexed(const vp_can_rule_t *rules, const uint8_t *order,
                              uint16_t count, uint32_t id)
{
    if (count < VP_CAN_INDEX_MIN) return visit_linear(rules, order, count, id);
    uint64_t result = 0;
    for (uint16_t k = vp_can_index_first(rules, order, count, id); k < count; k++) {
        const vp_can_rule_t *r = &rules[order[k]];
        if (r->can_id != id) break;
        result = result * 17u + r->src + 1u;
    }
    return result;
}
