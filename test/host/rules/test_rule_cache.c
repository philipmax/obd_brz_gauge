/* Compare cache reuse with forced output rebuilding. Golden editor vectors
 * independently cover condition, sorting and action semantics. This trace
 * checks the optimization cannot skip runtime evolution or retain stale output.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tr_eval.h"

#define N_VALUES 12
#define N_TARGETS (TR_MAX_RULES * TR_MAX_ACTIONS)

static unsigned checks, failures;
static uint32_t rng = UINT32_C(0x63caf129);

#define CHECK(x) do { \
    checks++; \
    if (!(x)) { \
        if (failures < 8) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        failures++; \
    } \
} while (0)

static uint32_t random_u32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static double priority(void)
{
    static const double values[] = {0, -0.0, 1, -1, 8, -8, NAN, INFINITY, -INFINITY};
    return values[random_u32() % (sizeof(values) / sizeof(values[0]))];
}

static void compare(const tr_program_t *cached, const tr_program_t *rebuilt)
{
    CHECK(cached->result.n_active == rebuilt->result.n_active);
    CHECK(cached->result.n_targets == rebuilt->result.n_targets);
    CHECK(!memcmp(cached->result.active, rebuilt->result.active, cached->result.n_active));
    CHECK(!memcmp(&cached->state, &rebuilt->state, sizeof(cached->state)));
    CHECK(!memcmp(cached->ovr, rebuilt->ovr, cached->n_targets * sizeof(*cached->ovr)));
    for (uint16_t t = 0; t < cached->n_targets; t++) {
        const tr_override_t *a = tr_result_override(cached, t);
        const tr_override_t *b = tr_result_override(rebuilt, t);
        CHECK((a == NULL) == (b == NULL));
    }
}

int main(void)
{
    static const char *const strings[] = {NULL, "", "text", "asset", "#ff3158"};
    static const double bands[] = {0, 1, -1, 4, NAN};
    static const double delays[] = {0, 1, 40, 120, NAN, INFINITY};
    unsigned reuse = 0, transitions = 0;
    for (unsigned page = 0; page < 128; page++) {
        tr_rule_t rules[TR_MAX_RULES] = {0};
        tr_cond_t conds[TR_MAX_RULES][TR_MAX_CONDS] = {0};
        tr_action_t actions[TR_MAX_RULES][TR_MAX_ACTIONS] = {0};
        tr_override_t cached_ovr[N_TARGETS] = {0}, rebuilt_ovr[N_TARGETS] = {0};
        tr_program_t cached = {0}, rebuilt = {0};
        cached.n_rules = (uint8_t)(page % (TR_MAX_RULES + 1));
        cached.n_targets = page == 0 ? 0 : (uint16_t)(1 + random_u32() % N_TARGETS);
        cached.rules = rules;
        cached.ovr = cached_ovr;
        rebuilt = cached;
        rebuilt.ovr = rebuilt_ovr;
        for (uint8_t i = 0; i < cached.n_rules; i++) {
            tr_rule_t *r = &rules[i];
            r->slot = page & 1u ? (uint8_t)(i / 2) : i; // repeated IDs share state
            r->trigger = (uint8_t)(random_u32() % 3);
            r->disabled = random_u32() % 9 == 0;
            r->any = (uint8_t)(random_u32() % 2);
            r->n_conds = (uint8_t)(random_u32() % (TR_MAX_CONDS + 1));
            r->n_actions = cached.n_targets ? (uint8_t)(random_u32() % (TR_MAX_ACTIONS + 1)) : 0;
            r->change_src = i == 0 ? TR_SRC_NONE : (uint16_t)(random_u32() % 70);
            r->interval_ms = 100.0 * (1 + random_u32() % 20);
            r->hysteresis = bands[random_u32() % (sizeof(bands) / sizeof(bands[0]))];
            r->debounce_ms = delays[random_u32() % (sizeof(delays) / sizeof(delays[0]))];
            r->priority = priority();
            r->conds = conds[i];
            r->actions = actions[i];
            for (uint8_t j = 0; j < r->n_conds; j++) {
                tr_cond_t *c = &conds[i][j];
                c->src = j == 0 && i % 7 == 0 ? TR_SRC_NONE : (uint16_t)(random_u32() % N_VALUES);
                c->cmp_src = (uint16_t)(random_u32() % (N_VALUES + 3));
                c->has_cmp = (uint8_t)(random_u32() % 2);
                c->op = (uint8_t)(random_u32() % (TR_OP_NE + 1));
                c->value_tag = (uint8_t)(random_u32() % (TR_VAL_OTHER + 1));
                c->value = (double)(random_u32() % 7) - 3.0;
            }
            for (uint8_t j = 0; j < r->n_actions; j++) {
                tr_action_t *a = &actions[i][j];
                a->target = (uint16_t)(random_u32() % cached.n_targets);
                a->props = (uint8_t)(random_u32() & 63u);
                a->visible = (uint8_t)(random_u32() % 2);
                a->color = random_u32() & UINT32_C(0xffffff);
                a->color_str = strings[random_u32() % 5];
                a->text = strings[random_u32() % 5];
                a->asset = strings[random_u32() % 5];
            }
        }
        tr_reset(&cached);
        tr_reset(&rebuilt);
        CHECK(!cached.have_resolved);
        double values[N_VALUES] = {0};
        tr_inputs_t in = {values, N_VALUES, 0, 0, 0};
        for (unsigned step = 0; step < 600; step++) {
            if (step % 137 == 0) {
                tr_reset(&cached);
                tr_reset(&rebuilt);
                CHECK(!cached.have_resolved);
            }
            // Stable bursts exercise reuse; changing inputs exercise exits,
            // hysteresis, shared slots, timer windows and on-delay progression.
            if (step % 13 == 0) {
                for (unsigned v = 0; v < N_VALUES; v++) {
                    values[v] = random_u32() % 8 == 0 ? NAN : (double)(random_u32() % 11) - 5.0;
                }
                in.invalid_mask = random_u32() & UINT32_C(0xfff);
                in.changed_mask = (uint64_t)random_u32() << 32 | random_u32();
                in.n_values = (uint16_t)(random_u32() % (N_VALUES + 1));
            }
            in.elapsed_ms += random_u32() % 34;
            if (step == 299) in.elapsed_ms = 0; // clock restart without state reset
            if (step % 29 == 0 && cached.n_rules) {
                uint8_t i = (uint8_t)(random_u32() % cached.n_rules);
                CHECK(tr_rule_active(&cached, i, &in) == tr_rule_active(&rebuilt, i, &in));
            }
            const uint32_t previous = cached.resolved_mask;
            const bool had_previous = cached.have_resolved;
            CHECK(tr_eval(&cached, &in) == &cached.result);
            rebuilt.have_resolved = false;
            CHECK(tr_eval(&rebuilt, &in) == &rebuilt.result);
            CHECK(cached.have_resolved);
            CHECK(cached.resolved_mask == rebuilt.resolved_mask);
            compare(&cached, &rebuilt);
            if (had_previous && cached.resolved_mask == previous) reuse++;
            else transitions++;
        }
        // Returning to no active rules must clear every previous override.
        for (uint8_t i = 0; i < cached.n_rules; i++) rules[i].disabled = 1;
        tr_reset(&cached);
        tr_reset(&rebuilt);
        CHECK(tr_eval(&cached, &in) != NULL);
        CHECK(tr_eval(&rebuilt, &in) != NULL);
        CHECK(cached.result.n_active == 0);
        compare(&cached, &rebuilt);
    }
    CHECK(reuse > 10000);
    CHECK(transitions > 1000);
    printf("rule cache checks=%u failures=%u reuse=%u rebuilds=%u\n", checks, failures, reuse, transitions);
    return failures ? 1 : 0;
}
