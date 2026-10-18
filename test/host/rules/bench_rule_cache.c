/* Optional release-only microbenchmark: same evaluator with normal caching
 * versus forced output rebuilding. The latter includes the cache-check/mask
 * overhead, so it is a recomputation comparison, not the old firmware timing.
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tr_eval.h"

#define SAMPLES 7
#define CALLS 300000
#define TARGETS (TR_MAX_RULES * TR_MAX_ACTIONS)

typedef const tr_result_t *(*eval_fn)(tr_program_t *, const tr_inputs_t *);

static const tr_result_t *cached_eval(tr_program_t *p, const tr_inputs_t *in)
{
    return tr_eval(p, in);
}

static const tr_result_t *rebuild_eval(tr_program_t *p, const tr_inputs_t *in)
{
    p->have_resolved = false;
    return tr_eval(p, in);
}

static uint64_t clock_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static double measure(eval_fn eval, tr_program_t *p, unsigned count, bool churn, uint64_t *sum)
{
    tr_inputs_t in = {NULL, 0, 0, UINT64_C(0xffff), 1};
    uint64_t checksum = 0;
    tr_reset(p);
    const uint64_t start = clock_ns();
    for (unsigned i = 0; i < count; i++) {
        in.elapsed_ms = (double)i;
        in.changed_mask = churn ? (i & 1u ? UINT64_C(0xaaaa) : UINT64_C(0x5555)) : UINT64_C(0xffff);
        const tr_result_t *res = eval(p, &in);
        checksum += res->n_active;
        if (res->n_active) checksum += res->active[res->n_active - 1];
        if (p->n_targets) checksum += p->ovr[p->n_targets - 1].color;
    }
    const uint64_t elapsed = clock_ns() - start;
    *sum = checksum;
    return (double)elapsed / count;
}

static double median(double values[SAMPLES])
{
    for (unsigned i = 1; i < SAMPLES; i++) {
        double x = values[i];
        unsigned j = i;
        while (j && x < values[j - 1]) { values[j] = values[j - 1]; j--; }
        values[j] = x;
    }
    return values[SAMPLES / 2];
}

static void scenario(uint8_t n_rules, bool churn, uint32_t seed)
{
    tr_program_t p = {0};
    tr_rule_t rules[TR_MAX_RULES] = {0};
    tr_action_t actions[TR_MAX_RULES][TR_MAX_ACTIONS] = {0};
    tr_override_t ovr[TARGETS] = {0};
    p.n_rules = n_rules;
    p.n_targets = (uint16_t)(n_rules * TR_MAX_ACTIONS);
    p.rules = rules;
    p.ovr = ovr;
    for (uint8_t i = 0; i < n_rules; i++) {
        tr_rule_t *r = &rules[i];
        r->trigger = TR_TRIG_CHANGE;
        r->change_src = i;
        r->slot = i;
        r->priority = (double)((seed ^ ((uint32_t)i * UINT32_C(2654435761))) & 31u);
        r->n_actions = TR_MAX_ACTIONS;
        r->actions = actions[i];
        for (uint8_t j = 0; j < TR_MAX_ACTIONS; j++) {
            tr_action_t *a = &actions[i][j];
            a->target = (uint16_t)(i * TR_MAX_ACTIONS + j);
            a->props = TR_PROP_COLOR | TR_PROP_VISIBLE | TR_PROP_TEXT | TR_PROP_ASSET;
            a->color = (seed ^ ((uint32_t)a->target * 73u)) & UINT32_C(0xffffff);
            a->visible = 1;
            a->text = "value";
            a->asset = "image";
        }
    }
    uint64_t sum_cache, sum_rebuild;
    (void)measure(cached_eval, &p, 10000, churn, &sum_cache);
    (void)measure(rebuild_eval, &p, 10000, churn, &sum_rebuild);
    double cache[SAMPLES], rebuild[SAMPLES];
    for (unsigned sample = 0; sample < SAMPLES; sample++) {
        if (sample & 1u) {
            cache[sample] = measure(cached_eval, &p, CALLS, churn, &sum_cache);
            rebuild[sample] = measure(rebuild_eval, &p, CALLS, churn, &sum_rebuild);
        } else {
            rebuild[sample] = measure(rebuild_eval, &p, CALLS, churn, &sum_rebuild);
            cache[sample] = measure(cached_eval, &p, CALLS, churn, &sum_cache);
        }
        if (sum_cache != sum_rebuild) {
            fprintf(stderr, "checksum mismatch\n");
            exit(1);
        }
    }
    double cached_ns = median(cache), rebuild_ns = median(rebuild);
    printf("%u,%u,%s,%.2f,%.2f,%.2f,%" PRIu64 "\n", n_rules, p.n_targets,
           churn ? "alternating" : "stable", rebuild_ns, cached_ns, rebuild_ns / cached_ns, sum_cache);
}

int main(int argc, char **argv)
{
    uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : UINT32_C(0x63caf129);
    fprintf(stderr, "seed=0x%08" PRIx32 " samples=%u calls=%u\n", seed, SAMPLES, CALLS);
    puts("rules,targets,active_set,rebuild_ns,cached_ns,ratio,checksum");
    scenario(0, false, seed);
    scenario(1, false, seed);
    scenario(8, false, seed);
    scenario(16, false, seed);
    scenario(16, true, seed);
    return 0;
}
