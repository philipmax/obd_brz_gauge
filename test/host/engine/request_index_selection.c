#include "request_index_selection.h"

#include <stddef.h>

#define RI_REQ 1u

void ri_build_index(ri_case_t *c)
{
    uint8_t tail[RI_MAX_REQUESTS];
    uint8_t i;

    for (i = 0; i < RI_MAX_REQUESTS; i++) {
        c->head[i] = RI_NONE;
        tail[i] = RI_NONE;
    }
    for (i = 0; i < RI_MAX_SOURCES; i++) c->next[i] = RI_NONE;
    for (i = 0; i < c->n_sig; i++) {
        const ri_signal_t *sig = &c->sig[i];
        uint8_t k;
        if (!sig->driven) continue;
        for (k = 0; k < sig->count; k++) {
            uint16_t si = (uint16_t)(sig->first + k);
            const ri_source_t *src = &c->src[si];
            uint8_t r;
            if (src->kind != RI_REQ) continue;
            r = src->ref;
            if (c->head[r] == RI_NONE) c->head[r] = (uint8_t)si;
            else c->next[tail[r]] = (uint8_t)si;
            tail[r] = (uint8_t)si;
        }
    }
}

uint32_t ri_dispatch_linear(const ri_case_t *c, uint8_t request)
{
    uint32_t sum = 0;
    uint8_t i;

    for (i = 0; i < c->n_sig; i++) {
        const ri_signal_t *sig = &c->sig[i];
        uint16_t si;
        const ri_source_t *src;
        if (!sig->driven || sig->count == 0) continue;
        si = (uint16_t)(sig->first + sig->cur);
        src = &c->src[si];
        if (src->kind == RI_REQ && src->ref == request) sum += (uint32_t)si + 1u;
    }
    return sum;
}

uint32_t ri_dispatch_indexed(const ri_case_t *c, uint8_t request)
{
    uint32_t sum = 0;
    uint8_t si;

    for (si = c->head[request]; si != RI_NONE; si = c->next[si]) {
        const ri_source_t *src = &c->src[si];
        const ri_signal_t *sig = &c->sig[src->sig];
        uint16_t current;
        if (!sig->driven || sig->count == 0) continue;
        current = (uint16_t)(sig->first + sig->cur);
        if (current == si) sum += (uint32_t)si + 1u;
    }
    return sum;
}
