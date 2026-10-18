/* Stable real-LVGL page updates, with and without a forced target pass.
 * Both paths retain the current evaluator/cache checks, binding update and
 * simulated signal snapshot. This is not old firmware or device frame time. */
#define _POSIX_C_SOURCE 200809L
#define main page_tests_main
#include "test_page.c"
#undef main

#define TARGETS (TR_MAX_RULES * TR_MAX_ACTIONS)
#define ITERATIONS 200000u
#define REPEATS 7

typedef struct {
    te_ctx_t ctx;
    te_live_page_t page;
    tl_layout_t layout;
    tl_elem_t elements[TARGETS];
    te_item_t items[TARGETS];
    uint16_t targets[TARGETS];
    tr_program_t prog;
    tr_target_t rule_targets[TARGETS];
    tr_override_t overrides[TARGETS];
    tr_rule_t rule[TR_MAX_RULES];
    tr_action_t actions[TARGETS];
    tr_cond_t condition;
} bench_fixture_t;

static void bench_init(bench_fixture_t *f, unsigned count, bool suppressed)
{
    memset(f, 0, sizeof(*f));
    g_te = &f->ctx;
    f->ctx.suppressed = suppressed;
    te_live_page_t *p = &f->page;
    p->layout = &f->layout; p->items = f->items; p->n_items = (uint16_t)count;
    p->tgt_item = f->targets; p->used_mask = 1; p->was_suppressed = suppressed;
    p->scr = lv_obj_create(NULL); p->prog = &f->prog;
    f->prog.n_targets = (uint16_t)count;
    f->prog.n_rules = (uint8_t)((count + TR_MAX_ACTIONS - 1) / TR_MAX_ACTIONS);
    f->prog.rules = f->rule; f->prog.targets = f->rule_targets; f->prog.ovr = f->overrides;
    f->condition = (tr_cond_t){.src=0, .op=TR_OP_GT, .value_tag=TR_VAL_NUM, .value=50};
    for (unsigned r = 0; r < f->prog.n_rules; r++) {
        unsigned first = r * TR_MAX_ACTIONS;
        unsigned n = count - first;
        if (n > TR_MAX_ACTIONS) n = TR_MAX_ACTIONS;
        f->rule[r] = (tr_rule_t){.slot=(uint8_t)r, .n_conds=1, .conds=&f->condition,
                                .n_actions=(uint8_t)n, .actions=&f->actions[first]};
    }
    for (unsigned i = 0; i < count; i++) {
        tl_elem_t *el = &f->elements[i];
        te_item_t *it = &f->items[i];
        el->kind = i % 3 == 0 ? TR_KIND_ARC : i % 3 == 1 ? TR_KIND_BAR : TR_KIND_LABEL;
        el->width = 100; el->height = 12; el->color = 0xeeeeee; el->bg_color = 0x222222;
        el->start_angle = 0; el->end_angle = 270; el->line_width = 2;
        el->text = "BASE"; el->format = "%.0f"; el->divisor = 1;
        it->el = el; it->kind = el->kind; it->lo = 0; it->hi = 100;
        it->slot = i < TL_MAX_BINDINGS ? 0 : -1; it->bound = it->slot >= 0;
        it->last_px = INT32_MIN;
        it->base = (te_look_t){.c1=el->color, .c2=el->bg_color, .text=el->text};
        it->applied = it->base;
        f->targets[i] = (uint16_t)i;
        f->rule_targets[i] = (tr_target_t){.elem=(uint16_t)i, .kind=el->kind};
        f->actions[i] = (tr_action_t){.target=(uint16_t)i, .props=TR_PROP_COLOR, .color=0xff0000};
        if (el->kind == TR_KIND_LABEL) { f->actions[i].props |= TR_PROP_TEXT; f->actions[i].text = "HOT"; }
        switch (el->kind) {
        case TR_KIND_ARC: te_create_arc(p->scr, it); break;
        case TR_KIND_BAR: te_create_bar(p->scr, it); break;
        default: te_create_label(p->scr, it); break;
        }
    }
    for (unsigned i = 0; i < SIG_SLOT_COUNT; i++) p->prev[i] = NAN;
    tr_reset(&f->prog);
    te_page_update(&f->ctx, p);
}
static void skipped(bench_fixture_t *f) { te_page_update(&f->ctx, &f->page); }
static void forced(bench_fixture_t *f)
{
    te_page_update(&f->ctx, &f->page);
    te_apply_targets(&f->page, &f->prog.result, f->ctx.suppressed);
}
static uint64_t time_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
typedef void (*update_fn)(bench_fixture_t *);
static double measure(update_fn update, bench_fixture_t *f, uint64_t *sum)
{
    uint64_t checksum = 0;
    for (unsigned i = 0; i < 1000; i++) update(f);
    uint64_t start = time_ns();
    for (unsigned i = 0; i < ITERATIONS; i++) {
        update(f);
        checksum += f->prog.resolved_mask;
        checksum += (uint64_t)f->page.values[0];
    }
    uint64_t elapsed = time_ns() - start;
    *sum = checksum;
    return (double)elapsed / ITERATIONS;
}
static double median(double a[REPEATS])
{
    for (unsigned i = 1; i < REPEATS; i++) {
        double v = a[i]; unsigned j = i;
        while (j && a[j-1] > v) { a[j] = a[j-1]; j--; }
        a[j] = v;
    }
    return a[REPEATS / 2];
}
int main(void)
{
    lv_init();
    static lv_color_t buf[360 * 8];
    static lv_disp_draw_buf_t draw;
    static lv_disp_drv_t drv;
    lv_disp_draw_buf_init(&draw, buf, NULL, 360 * 8);
    lv_disp_drv_init(&drv); drv.hor_res = drv.ver_res = 360;
    drv.draw_buf = &draw; drv.flush_cb = flush;
    lv_disp_t *disp = lv_disp_drv_register(&drv);
    static const unsigned counts[] = {0, 1, 8, 64, 128};
    printf("targets,active,suppressed,forced_targets_ns,skipped_targets_ns,ratio,checksum\n");
    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        for (unsigned active = 0; active < 2; active++) for (unsigned sup = 0; sup < 2; sup++) {
            bench_fixture_t *f = calloc(1, sizeof(*f));
            if (!f) abort();
            clock_ms = 1; samples[0] = active ? 90 : 10;
            bench_init(f, counts[c], sup != 0);
            double baseline[REPEATS], optimized[REPEATS];
            uint64_t sb = 0, so = 0;
            for (unsigned rep = 0; rep < REPEATS; rep++) {
                if (rep & 1u) {
                    optimized[rep] = measure(skipped, f, &so);
                    baseline[rep] = measure(forced, f, &sb);
                } else {
                    baseline[rep] = measure(forced, f, &sb);
                    optimized[rep] = measure(skipped, f, &so);
                }
                if (sb != so) { fprintf(stderr, "checksum mismatch\n"); abort(); }
            }
            double b = median(baseline), o = median(optimized);
            printf("%u,%u,%u,%.2f,%.2f,%.2f,%llu\n", counts[c], active, sup, b, o, b/o, (unsigned long long)so);
            lv_obj_del(f->page.scr); free(f);
        }
    }
    lv_disp_remove(disp);
    return 0;
}
