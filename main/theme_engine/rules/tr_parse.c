#include "tr_parse.h"
#include "tr_eval.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Every block of the program allocation starts on this boundary.
#define TR_ALIGN 8
#define TR_ALIGN_UP(n) (((n) + (TR_ALIGN - 1)) & ~(size_t)(TR_ALIGN - 1))

// Two passes over the same JSON: the dry pass sizes the single
// allocation, the fill pass writes into it.
typedef struct {
    bool dry;
    const tr_compile_cfg_t *cfg;
    const cJSON *page_elements;   // page.elements[] when cfg->elements is NULL

    size_t n_rules, n_conds, n_actions, n_targets, pool_used;

    tr_rule_t   *rules;
    tr_cond_t   *conds;
    tr_action_t *actions;
    tr_target_t *targets;
    char        *pool;
    size_t       pool_cap;

    tr_diag_t diag;
} builder_t;

// ---------------- JS value helpers ----------------

// JSON.parse keeps the last duplicate key; lookups are case-sensitive.
static const cJSON *jget(const cJSON *obj, const char *key)
{
    const cJSON *hit = NULL;
    if (!cJSON_IsObject(obj)) return NULL;
    for (const cJSON *c = obj->child; c; c = c->next) {
        if (c->string && strcmp(c->string, key) == 0) hit = c;
    }
    return hit;
}

static bool js_nullish(const cJSON *it)
{
    return it == NULL || cJSON_IsNull(it);
}

static bool js_truthy(const cJSON *it)
{
    if (js_nullish(it) || cJSON_IsFalse(it)) return false;
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0 && !isnan(it->valuedouble);
    if (cJSON_IsString(it)) return it->valuestring && it->valuestring[0];
    return true;
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// StringToNumber for ASCII input: trim, "" → 0, 0x/0o/0b, Infinity, decimals.
static double js_str_to_number(const char *s)
{
    const char *b = s, *e = s + strlen(s);
    while (b < e && is_ws(*b)) b++;
    while (e > b && is_ws(e[-1])) e--;
    if (b == e) return 0.0;

    if (e - b > 2 && b[0] == '0') {
        int radix = 0;
        char p = (char)(b[1] | 0x20);
        if (p == 'x') radix = 16;
        else if (p == 'o') radix = 8;
        else if (p == 'b') radix = 2;
        if (radix) {
            double v = 0.0;
            for (const char *q = b + 2; q < e; q++) {
                int d = hex_digit(*q);
                if (d < 0 || d >= radix) return NAN;
                v = v * radix + d;
            }
            return v;
        }
    }

    const char *p = b;
    if (*p == '+' || *p == '-') p++;
    if (e - p == 8 && memcmp(p, "Infinity", 8) == 0) {
        return *b == '-' ? -INFINITY : INFINITY;
    }
    const char *q = p;
    int digits = 0;
    while (q < e && is_digit(*q)) { q++; digits++; }
    if (q < e && *q == '.') {
        q++;
        while (q < e && is_digit(*q)) { q++; digits++; }
    }
    if (digits == 0) return NAN;
    if (q < e && (*q == 'e' || *q == 'E')) {
        q++;
        if (q < e && (*q == '+' || *q == '-')) q++;
        const char *d0 = q;
        while (q < e && is_digit(*q)) q++;
        if (q == d0) return NAN;
    }
    if (q != e) return NAN;
    return strtod(b, NULL);   // grammar checked: plain decimal, ends at e
}

// ToNumber of a JSON value; absent (undefined) → NaN.
// Arrays and objects are approximated as NaN.
static double js_to_number(const cJSON *it)
{
    if (!it) return NAN;
    if (cJSON_IsNumber(it)) return it->valuedouble;
    if (cJSON_IsTrue(it)) return 1.0;
    if (cJSON_IsFalse(it) || cJSON_IsNull(it)) return 0.0;
    if (cJSON_IsString(it)) return js_str_to_number(it->valuestring);
    return NAN;
}

// `x ?? dflt` used numerically
static double js_num_or(const cJSON *it, double dflt)
{
    return js_nullish(it) ? dflt : js_to_number(it);
}

// Math.max: NaN wins (unlike fmax)
static double js_max(double a, double b)
{
    if (isnan(a) || isnan(b)) return NAN;
    return a > b ? a : b;
}

static double js_min(double a, double b)
{
    if (isnan(a) || isnan(b)) return NAN;
    return a < b ? a : b;
}

// ---------------- public helpers ----------------

bool tr_parse_color(const char *s, uint32_t *rgb)
{
    if (!s) return false;
    const char *h = s;
    if (h[0] == '#') h++;
    else if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;

    size_t n = strlen(h);
    uint32_t v = 0;
    if (n == 6) {
        for (size_t i = 0; i < 6; i++) {
            int d = hex_digit(h[i]);
            if (d < 0) return false;
            v = (v << 4) | (uint32_t)d;
        }
    } else if (n == 3 && s[0] == '#') {
        for (size_t i = 0; i < 3; i++) {
            int d = hex_digit(h[i]);
            if (d < 0) return false;
            v = (v << 8) | (uint32_t)(d * 17);
        }
    } else {
        return false;
    }
    if (rgb) *rgb = v;
    return true;
}

tr_kind_t tr_kind_from_str(const char *s)
{
    if (!s) return TR_KIND_UNKNOWN;
    if (strcmp(s, "arc") == 0) return TR_KIND_ARC;
    if (strcmp(s, "bar") == 0) return TR_KIND_BAR;
    if (strcmp(s, "label") == 0) return TR_KIND_LABEL;
    if (strcmp(s, "image") == 0) return TR_KIND_IMAGE;
    if (strcmp(s, "pointer") == 0) return TR_KIND_POINTER;
    return TR_KIND_UNKNOWN;
}

// ---------------- builder ----------------

static const char *pool_dup(builder_t *b, const char *s)
{
    size_t n = strlen(s) + 1;
    if (b->dry) {
        b->pool_used += n;
        return "";
    }
    if (b->pool_used + n > b->pool_cap) return "";   // cannot happen: sized by the dry pass
    char *d = b->pool + b->pool_used;
    memcpy(d, s, n);
    b->pool_used += n;
    return d;
}

static uint16_t resolve_key(builder_t *b, const char *key)
{
    int idx = -1;
    if (b->cfg->resolve_source) idx = b->cfg->resolve_source(key, b->cfg->resolve_ctx);
    if (idx < 0 || idx >= (int)TR_SRC_NONE) {
        b->diag.unknown_sources++;
        return TR_SRC_NONE;
    }
    return (uint16_t)idx;
}

// values[key]: only string keys can name a source
static uint16_t resolve(builder_t *b, const cJSON *key)
{
    if (!cJSON_IsString(key) || !key->valuestring) return TR_SRC_NONE;
    return resolve_key(b, key->valuestring);
}

static tr_op_t op_from(const cJSON *it)
{
    if (!cJSON_IsString(it) || !it->valuestring) return TR_OP_NONE;
    const char *s = it->valuestring;
    if (strcmp(s, ">") == 0) return TR_OP_GT;
    if (strcmp(s, ">=") == 0) return TR_OP_GE;
    if (strcmp(s, "<") == 0) return TR_OP_LT;
    if (strcmp(s, "<=") == 0) return TR_OP_LE;
    if (strcmp(s, "==") == 0) return TR_OP_EQ;
    if (strcmp(s, "!=") == 0) return TR_OP_NE;
    return TR_OP_NONE;
}

// cond may be any JSON value; non-objects read as all-undefined.
static void build_cond(builder_t *b, const cJSON *cond)
{
    if (b->dry) {
        b->n_conds++;
        return;
    }
    tr_cond_t *k = &b->conds[b->n_conds++];
    k->src = resolve(b, jget(cond, "source"));
    k->op = (uint8_t)op_from(jget(cond, "op"));

    const cJSON *cs = jget(cond, "compareSource");
    k->has_cmp = js_truthy(cs) ? 1 : 0;
    k->cmp_src = k->has_cmp ? resolve(b, cs) : TR_SRC_NONE;

    const cJSON *v = jget(cond, "value");
    if (!v) {
        k->value_tag = TR_VAL_UNDEF;
        k->value = NAN;
    } else if (cJSON_IsNull(v)) {
        k->value_tag = TR_VAL_NULL;
        k->value = 0.0;
    } else if (cJSON_IsNumber(v)) {
        k->value_tag = TR_VAL_NUM;
        k->value = v->valuedouble;
    } else {
        k->value_tag = TR_VAL_OTHER;
        k->value = js_to_number(v);
    }
}

static bool lookup_element(builder_t *b, const char *id, uint16_t *elem, tr_kind_t *kind)
{
    const tr_compile_cfg_t *cfg = b->cfg;
    if (cfg->elements) {
        for (uint16_t i = 0; i < cfg->n_elements; i++) {
            if (cfg->elements[i].id && strcmp(cfg->elements[i].id, id) == 0) {
                *elem = i;
                *kind = cfg->elements[i].kind;
                return true;
            }
        }
        return false;
    }
    uint16_t i = 0;
    for (const cJSON *el = cJSON_IsArray(b->page_elements) ? b->page_elements->child : NULL;
         el && i < TR_ELEM_NONE; el = el->next, i++) {
        const cJSON *eid = jget(el, "id");
        if (cJSON_IsString(eid) && eid->valuestring && strcmp(eid->valuestring, id) == 0) {
            const cJSON *k = jget(el, "kind");
            *elem = i;
            *kind = cJSON_IsString(k) ? tr_kind_from_str(k->valuestring) : TR_KIND_UNKNOWN;
            return true;
        }
    }
    return false;
}

static uint16_t target_index(builder_t *b, const char *id)
{
    for (size_t i = 0; i < b->n_targets; i++) {
        if (strcmp(b->targets[i].id, id) == 0) return (uint16_t)i;
    }
    tr_target_t *t = &b->targets[b->n_targets];
    uint16_t elem = TR_ELEM_NONE;
    tr_kind_t kind = TR_KIND_UNKNOWN;
    if (!lookup_element(b, id, &elem, &kind)) b->diag.unknown_targets++;
    t->id = pool_dup(b, id);
    t->elem = elem;
    t->kind = (uint8_t)kind;
    return (uint16_t)b->n_targets++;
}

static uint8_t color_props(tr_kind_t kind)
{
    switch (kind) {
    case TR_KIND_IMAGE:
    case TR_KIND_POINTER: return TR_PROP_TINT;
    case TR_KIND_ARC:
    case TR_KIND_BAR:     return TR_PROP_COLOR;
    default:              return TR_PROP_COLOR;
    }
}

static void parse_animation(const cJSON *json, tr_animation_t *out)
{
    if (!cJSON_IsObject(json)) return;
    const cJSON *mode = jget(json, "mode");
    if (!cJSON_IsString(mode) || !mode->valuestring) return;
    if (strcmp(mode->valuestring, "fade") == 0) out->mode = 1;
    else if (strcmp(mode->valuestring, "keyframes") == 0) out->mode = 2;
    else if (strcmp(mode->valuestring, "strobe") == 0) out->mode = 3;
    else return;
    out->duration_ms = js_max(100.0, js_num_or(jget(json, "durationMs"), 1000.0));
    out->frequency_hz = js_max(0.5, js_num_or(jget(json, "frequencyHz"), 4.0));
    if (out->frequency_hz > 8.0) out->frequency_hz = 8.0;
    const cJSON *frames = jget(json, "keyframes");
    if (out->mode != 2 || !cJSON_IsArray(frames)) return;
    for (const cJSON *f = frames->child; f && out->n_frames < TR_MAX_KEYFRAMES; f = f->next) {
        const cJSON *at = jget(f, "at");
        const cJSON *value = jget(f, "value");
        if (!cJSON_IsNumber(at) || !cJSON_IsNumber(value)) continue;
        out->frame_at[out->n_frames] = js_max(0.0, js_min(100.0, at->valuedouble)) / 100.0;
        out->frame_value[out->n_frames] = js_max(0.0, js_min(100.0, value->valuedouble)) / 100.0;
        out->n_frames++;
    }
    // Editor defaults to a linear 0..100 curve when no usable frames exist.
    if (!out->n_frames) {
        out->n_frames = 2;
        out->frame_at[0] = out->frame_value[0] = 0.0;
        out->frame_at[1] = out->frame_value[1] = 1.0;
    }
}

// Returns false when the action is not kept (no string target).
static bool build_action(builder_t *b, const cJSON *act)
{
    const cJSON *target = jget(act, "target");
    if (!cJSON_IsString(target) || !target->valuestring) {
        if (!b->dry) b->diag.actions_invalid++;
        return false;
    }
    const cJSON *set = jget(act, "set");
    const cJSON *color = jget(set, "color");
    const cJSON *text = jget(set, "text");
    const cJSON *visible = jget(set, "visible");
    const cJSON *asset = jget(set, "asset");
    const cJSON *animation = jget(act, "animation");

    if (b->dry) {
        b->n_actions++;
        pool_dup(b, target->valuestring);   // upper bound: every target unique
        if (cJSON_IsString(color) && color->valuestring) pool_dup(b, color->valuestring);
        if (cJSON_IsString(text) && text->valuestring) pool_dup(b, text->valuestring);
        if (cJSON_IsString(asset) && asset->valuestring) pool_dup(b, asset->valuestring);
        return true;
    }

    tr_action_t *a = &b->actions[b->n_actions++];
    parse_animation(animation, &a->animation);
    a->target = target_index(b, target->valuestring);
    tr_kind_t kind = (tr_kind_t)b->targets[a->target].kind;

    // set keys in the editor's order: color → text → visible → asset
    if (color) {
        uint32_t rgb;
        if (cJSON_IsString(color) && color->valuestring && tr_parse_color(color->valuestring, &rgb)) {
            a->props |= color_props(kind);
            a->color = rgb;
            a->color_str = pool_dup(b, color->valuestring);
        } else {
            b->diag.bad_values++;
        }
    }
    if (text) {
        if (cJSON_IsString(text) && text->valuestring) {
            a->props |= TR_PROP_TEXT;
            a->text = pool_dup(b, text->valuestring);
        } else {
            b->diag.bad_values++;
        }
    }
    if (visible) {
        a->props |= TR_PROP_VISIBLE;
        a->visible = cJSON_IsFalse(visible) ? 0 : 1;   // preview hides only on === false
    }
    if (asset) {
        if (cJSON_IsString(asset) && asset->valuestring) {
            a->props |= TR_PROP_ASSET;
            a->asset = pool_dup(b, asset->valuestring);
        } else {
            b->diag.bad_values++;
        }
    }
    return true;
}

static void build_rule(builder_t *b, const cJSON *jr)
{
    const cJSON *when = jget(jr, "when");   // non-object reads as {}
    const cJSON *jid = jget(jr, "id");
    const char *id_src = (cJSON_IsString(jid) && jid->valuestring) ? jid->valuestring : "";

    // getRuleConditions(): conditions[] if non-empty, else the legacy fields
    const cJSON *conds = jget(when, "conditions");
    bool legacy = false;
    int n_conds_json = 0;
    if (cJSON_IsArray(conds) && cJSON_GetArraySize(conds) > 0) {
        n_conds_json = cJSON_GetArraySize(conds);
    } else if (js_truthy(jget(when, "source")) && js_truthy(jget(when, "op")) &&
               jget(when, "value") != NULL) {
        legacy = true;
        n_conds_json = 1;
    }
    int n_conds = n_conds_json > TR_MAX_CONDS ? TR_MAX_CONDS : n_conds_json;

    const cJSON *apply = jget(jr, "apply");
    int n_apply_json = cJSON_IsArray(apply) ? cJSON_GetArraySize(apply) : 0;
    int n_apply = n_apply_json > TR_MAX_ACTIONS ? TR_MAX_ACTIONS : n_apply_json;

    size_t cond_base = b->n_conds;
    size_t act_base = b->n_actions;

    if (b->dry) {
        b->n_rules++;
        pool_dup(b, id_src);
    }

    if (legacy) {
        build_cond(b, when);
    } else {
        int i = 0;
        for (const cJSON *c = conds ? conds->child : NULL; c && i < n_conds; c = c->next, i++) {
            build_cond(b, c);
        }
    }

    int kept = 0, i = 0;
    for (const cJSON *a = n_apply ? apply->child : NULL; a && i < n_apply; a = a->next, i++) {
        if (build_action(b, a)) kept++;
    }

    if (b->dry) return;

    b->diag.conds_dropped += (uint16_t)(n_conds_json - n_conds);
    b->diag.actions_dropped += (uint16_t)(n_apply_json - n_apply);

    size_t ri = b->n_rules++;
    tr_rule_t *r = &b->rules[ri];
    r->id = pool_dup(b, id_src);
    r->slot = (uint8_t)ri;
    for (size_t j = 0; j < ri; j++) {
        if (strcmp(b->rules[j].id, r->id) == 0) {
            r->slot = b->rules[j].slot;
            b->diag.shared_ids++;
            break;
        }
    }

    const cJSON *en = jget(jr, "enabled");
    r->disabled = cJSON_IsFalse(en) ? 1 : 0;
    r->priority = js_num_or(jget(jr, "priority"), 0.0);
    r->debounce_ms = js_max(0.0, js_num_or(jget(jr, "debounceMs"), 0.0));
    r->any = 0;
    const cJSON *comb = jget(when, "combine");
    if (cJSON_IsString(comb) && comb->valuestring && strcmp(comb->valuestring, "any") == 0) r->any = 1;
    r->hysteresis = js_num_or(jget(when, "hysteresis"), 0.0);

    r->trigger = TR_TRIG_CONDITION;
    r->interval_ms = 1000.0;
    r->change_src = TR_SRC_NONE;
    const cJSON *trig = jget(jr, "trigger");
    const cJSON *type = jget(trig, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        if (strcmp(type->valuestring, "timer") == 0) {
            r->trigger = TR_TRIG_TIMER;
            r->interval_ms = js_max(100.0, js_num_or(jget(trig, "intervalMs"), 1000.0));
        } else if (strcmp(type->valuestring, "change") == 0) {
            r->trigger = TR_TRIG_CHANGE;
            const cJSON *src = jget(trig, "source");
            if (js_nullish(src)) src = jget(when, "source");
            r->change_src = js_nullish(src) ? resolve_key(b, "obd.rpm") : resolve(b, src);
            if (r->change_src != TR_SRC_NONE && r->change_src >= TR_MASK_SOURCES) {
                b->diag.change_unmasked++;
            }
        }
    }

    r->n_conds = (uint8_t)(b->n_conds - cond_base);
    r->conds = r->n_conds ? &b->conds[cond_base] : NULL;
    r->n_actions = (uint8_t)kept;
    r->actions = kept ? &b->actions[act_base] : NULL;
    for (uint8_t j = 0; j < r->n_actions; j++) {
        const tr_animation_t *anim = &r->actions[j].animation;
        if (anim->mode && anim->duration_ms > r->transient_hold_ms)
            r->transient_hold_ms = anim->duration_ms;
    }
}

static void build_page(builder_t *b, const cJSON *page)
{
    const cJSON *rules = jget(page, "rules");
    int total = cJSON_IsArray(rules) ? cJSON_GetArraySize(rules) : 0;
    if (!b->dry) b->diag.rules_in = (uint16_t)total;

    int compiled = 0;
    for (const cJSON *r = total ? rules->child : NULL; r; r = r->next) {
        if (compiled >= TR_MAX_RULES || !cJSON_IsObject(r)) {
            if (!b->dry) b->diag.rules_dropped++;
            continue;
        }
        build_rule(b, r);
        compiled++;
    }
}

static void *default_calloc(size_t n, size_t size, void *ctx)
{
    (void)ctx;
    return calloc(n, size);
}

static void default_free(void *ptr, void *ctx)
{
    (void)ctx;
    free(ptr);
}

tr_err_t tr_compile(const cJSON *page, const tr_compile_cfg_t *cfg,
                    tr_program_t **out, tr_diag_t *diag)
{
    if (!out || !cfg) return TR_ERR_ARG;
    *out = NULL;
    if (diag) memset(diag, 0, sizeof(*diag));
    if (!cJSON_IsObject(page)) return TR_ERR_JSON;

    tr_alloc_t alloc = cfg->alloc;
    if (!alloc.calloc_fn || !alloc.free_fn) {
        alloc.calloc_fn = default_calloc;
        alloc.free_fn = default_free;
        alloc.ctx = NULL;
    }

    builder_t b;
    memset(&b, 0, sizeof(b));
    b.dry = true;
    b.cfg = cfg;
    b.page_elements = jget(page, "elements");
    build_page(&b, page);

    size_t n_rules = b.n_rules, n_conds = b.n_conds, n_actions = b.n_actions;
    size_t pool_cap = b.pool_used;

    size_t off_rules = TR_ALIGN_UP(sizeof(tr_program_t));
    size_t off_conds = off_rules + TR_ALIGN_UP(n_rules * sizeof(tr_rule_t));
    size_t off_actions = off_conds + TR_ALIGN_UP(n_conds * sizeof(tr_cond_t));
    size_t off_targets = off_actions + TR_ALIGN_UP(n_actions * sizeof(tr_action_t));
    size_t off_ovr = off_targets + TR_ALIGN_UP(n_actions * sizeof(tr_target_t));
    size_t off_pool = off_ovr + TR_ALIGN_UP(n_actions * sizeof(tr_override_t));
    size_t total = off_pool + pool_cap;

    uint8_t *mem = alloc.calloc_fn(1, total, alloc.ctx);
    if (!mem) return TR_ERR_NOMEM;

    memset(&b, 0, sizeof(b));
    b.dry = false;
    b.cfg = cfg;
    b.page_elements = jget(page, "elements");
    b.rules = (tr_rule_t *)(void *)(mem + off_rules);
    b.conds = (tr_cond_t *)(void *)(mem + off_conds);
    b.actions = (tr_action_t *)(void *)(mem + off_actions);
    b.targets = (tr_target_t *)(void *)(mem + off_targets);
    b.pool = (char *)(mem + off_pool);
    b.pool_cap = pool_cap;
    build_page(&b, page);

    tr_program_t *p = (tr_program_t *)(void *)mem;
    p->n_rules = (uint8_t)b.n_rules;
    p->n_targets = (uint16_t)b.n_targets;
    p->rules = b.rules;
    p->targets = b.targets;
    p->ovr = (tr_override_t *)(void *)(mem + off_ovr);
    p->alloc = alloc;
    tr_reset(p);

    if (diag) *diag = b.diag;
    *out = p;
    return TR_OK;
}

tr_err_t tr_compile_json(const char *json, size_t len, const tr_compile_cfg_t *cfg,
                         tr_program_t **out, tr_diag_t *diag)
{
    if (!json || !out) return TR_ERR_ARG;
    *out = NULL;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        if (diag) memset(diag, 0, sizeof(*diag));
        return TR_ERR_JSON;
    }
    tr_err_t err = tr_compile(root, cfg, out, diag);
    cJSON_Delete(root);
    return err;
}

void tr_program_free(tr_program_t *prog)
{
    if (!prog) return;
    tr_alloc_t alloc = prog->alloc;
    alloc.free_fn(prog, alloc.ctx);
}
