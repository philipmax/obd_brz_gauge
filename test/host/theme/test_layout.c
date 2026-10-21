// theme_layout: theme.bin page layout → plain element table (R(v), colors
// with manifest fallbacks, limits, unknown types, allocator failures).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "theme_layout.h"
#include "th_util.h"

static int g_live;
static int g_fail_after = -1;   // fail the Nth allocation (0-based), -1 = never

static void *cnt_alloc(size_t n, void *ctx)
{
    (void)ctx;
    if (g_fail_after == 0) return NULL;
    if (g_fail_after > 0) g_fail_after--;
    g_live++;
    return malloc(n);
}

static void cnt_free(void *p, void *ctx)
{
    (void)ctx;
    if (p) g_live--;
    free(p);
}

static const tl_alloc_t A = {cnt_alloc, cnt_free, NULL};
static const tl_defaults_t D = {0x111111, 0x222222, 0x333333, 0x444444};
static char g_scratch[TL_ITEM_MAX];

static tl_layout_t *parse(const char *s)
{
    return tl_parse(s, strlen(s), &D, &A, g_scratch, sizeof(g_scratch));
}

static void test_round(void)
{
    TH_CHECK(tl_round(-0.5) == 0 && tl_round(0.5) == 1 && tl_round(-120) == -120);
    TH_CHECK(tl_round(-120.5) == -120 && tl_round(-120.51) == -121 && tl_round(2.5) == 3);
    TH_CHECK(tl_px(-0.5) == 0 && tl_px(179.5) == 180 && tl_px(-1.5) == -1);
    TH_CHECK(tl_px(1e9) == TL_PX_MAX && tl_px(-1e9) == -TL_PX_MAX && tl_px(NAN) == 0);
    const char *k = NULL;
    TH_CHECK(tl_is_video_id("vid_intro", &k) && strcmp(k, "intro") == 0);
    TH_CHECK(tl_is_video_id("video_x", &k) && strcmp(k, "x") == 0);
    TH_CHECK(!tl_is_video_id("vi_x", NULL) && !tl_is_video_id("Vid_x", NULL) && !tl_is_video_id(NULL, NULL));
}

static void test_elements(void)
{
    const char *s =
        "{\"page_id\":\"main_gauge\",\"background_color\":\"#0a0B0c\",\"elements\":["
        "{\"type\":\"arc\",\"id\":\"arc_1\",\"x\":-120.5,\"y\":10.25,\"width\":200,\"height\":180,"
        "\"start_angle\":135,\"end_angle\":45,\"rotation\":0,\"data_source\":\"obd.rpm\","
        "\"range_min\":0,\"range_max\":7000,\"color\":\"0xFF3158\",\"bg_color\":\"bad\",\"line_width\":12},"
        "{\"type\":\"bar\",\"id\":\"bar_1\",\"x\":1,\"y\":2,\"width\":100,\"height\":10,"
        "\"color\":\"00ff00\",\"bg_color\":\"#123\",\"radius\":4,\"segments\":5,\"extra\":{\"a\":[1,2]}},"
        "{\"type\":\"gauge3d\",\"id\":\"x\"},"
        "{\"type\":\"label\",\"id\":\"lbl_1\",\"x\":180,\"y\":200,\"align\":\"center\",\"color\":\"#zzzzzz\","
        "\"font_size\":64,\"data_source\":\"obd.speed\",\"format\":\"%d km/h\",\"divisor\":10,"
        "\"font_asset\":\"font_dseg_26\"},"
        "{\"type\":\"label\",\"id\":\"lbl_2\",\"x\":5,\"y\":6,\"align\":\"right\",\"text\":\"\\u00b0C\","
        "\"data_source\":\"\"},"
        "{\"type\":\"image\",\"id\":\"vid_intro\",\"x\":0,\"y\":0,\"width\":360,\"height\":360,"
        "\"rotation\":12.34,\"asset\":\"poster\",\"data_source\":\"obd.rpm\",\"opacity\":35},"
        "{\"type\":\"pointer\",\"id\":\"ptr_1\",\"x\":170,\"y\":40,\"width\":20,\"height\":150,"
        "\"asset\":\"needle\",\"data_source\":\"obd.rpm\",\"range_min\":0,\"range_max\":8000,"
        "\"angle_min\":-120,\"angle_max\":120,\"rotation\":90,\"pivot_x\":50,\"pivot_y\":90},"
        "42,"
        "{\"type\":\"image\",\"asset\":\"\"}"
        "]}";
    tl_layout_t *l = parse(s);
    TH_CHECK(l != NULL);
    if (!l) return;
    TH_CHECK(l->bg_color == 0x0A0B0C);
    TH_CHECK(l->n == 7 && l->unknown_types == 1 && l->invalid == 1 && l->over_limit == 0);
    TH_CHECK(strcmp(l->first_unknown, "gauge3d") == 0);

    const tl_elem_t *e = &l->el[0];
    TH_CHECK(e->kind == TR_KIND_ARC && strcmp(e->id, "arc_1") == 0);
    TH_CHECK(e->x == -120.5 && e->y == 10.25 && e->width == 200 && e->height == 180);
    TH_CHECK(e->start_angle == 135 && e->end_angle == 45 && e->line_width == 12);
    TH_CHECK(e->range_max == 7000 && strcmp(e->data_source, "obd.rpm") == 0);
    TH_CHECK(e->color == 0xFF3158 && e->bg_color == D.arc_track);

    e = &l->el[1];
    TH_CHECK(e->kind == TR_KIND_BAR && e->color == 0x00FF00 && e->bg_color == 0x112233);   // #rgb accepted
    TH_CHECK(e->radius == 4 && e->data_source == NULL && e->range_min == 0 && e->range_max == 100);

    e = &l->el[2];
    TH_CHECK(e->kind == TR_KIND_LABEL && e->align == TL_ALIGN_CENTER && e->color == D.text_primary);
    TH_CHECK(e->font_size == 64 && e->divisor == 10 && strcmp(e->format, "%d km/h") == 0);
    TH_CHECK(strcmp(e->font_asset, "font_dseg_26") == 0 && e->text == NULL);

    e = &l->el[3];
    TH_CHECK(e->align == TL_ALIGN_RIGHT && strcmp(e->text, "\xc2\xb0" "C") == 0);
    TH_CHECK(e->data_source == NULL && e->font_size == 16);

    e = &l->el[4];
    TH_CHECK(e->kind == TR_KIND_IMAGE && e->video && strcmp(e->asset, "poster") == 0);
    TH_CHECK(e->rotation == 12.34 && e->data_source == NULL && e->opacity == 35);   // images never bind

    e = &l->el[5];
    TH_CHECK(e->kind == TR_KIND_POINTER && !e->video && e->angle_min == -120 && e->angle_max == 120);
    TH_CHECK(e->pivot_y == 90 && e->rotation == 90 && strcmp(e->asset, "needle") == 0 && e->opacity == 100);

    e = &l->el[6];
    TH_CHECK(e->kind == TR_KIND_IMAGE && e->id[0] == '\0' && e->asset == NULL);
    tl_free(l, &A);
    TH_CHECK(g_live == 0);

    // no background_color, invalid background_color → defaults.bg
    l = parse("{\"elements\":[]}");
    TH_CHECK(l && l->n == 0 && l->bg_color == D.bg);
    tl_free(l, &A);
    l = parse("{\"background_color\":12,\"elements\":[]}");
    TH_CHECK(l && l->bg_color == D.bg);
    tl_free(l, &A);
    TH_CHECK(parse("[1,2]") == NULL && parse("nope") == NULL);
    TH_CHECK(g_live == 0);
}

static void test_limits(void)
{
    // 130 labels with data_source: 128 kept, 32 bound, 96 dropped bindings
    size_t cap = 130 * 96 + 64;
    char *s = malloc(cap);
    size_t n = (size_t)snprintf(s, cap, "{\"elements\":[");
    for (int i = 0; i < 130; i++) {
        n += (size_t)snprintf(s + n, cap - n, "%s{\"type\":\"label\",\"id\":\"l%d\",\"data_source\":\"obd.rpm\"}",
                              i ? "," : "", i);
    }
    snprintf(s + n, cap - n, "]}");
    tl_layout_t *l = parse(s);
    TH_CHECK(l && l->n == TL_MAX_ELEMENTS && l->over_limit == 2);
    TH_CHECK(l && l->bindings_dropped == TL_MAX_ELEMENTS - TL_MAX_BINDINGS);
    TH_CHECK(l && l->el[31].data_source != NULL && l->el[32].data_source == NULL);
    TH_CHECK(l && strcmp(l->el[127].id, "l127") == 0);
    tl_free(l, &A);
    free(s);

    // element larger than the scratch → invalid, the rest still parses
    char big[600];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    cap = sizeof(big) + 256;
    s = malloc(cap);
    snprintf(s, cap, "{\"elements\":[{\"type\":\"label\",\"text\":\"%s\"},{\"type\":\"bar\"}]}", big);
    char small[256];
    l = tl_parse(s, strlen(s), &D, &A, small, sizeof(small));
    TH_CHECK(l && l->n == 1 && l->invalid == 1 && l->el[0].kind == TR_KIND_BAR);
    tl_free(l, &A);

    // every allocation failure point: NULL, no leaks
    snprintf(s, cap, "{\"elements\":[{\"type\":\"label\",\"id\":\"a\",\"text\":\"t\",\"format\":\"%%d\","
                     "\"data_source\":\"obd.rpm\",\"font_asset\":\"f\"},{\"type\":\"image\",\"asset\":\"x\"}]}");
    int failures = 0;
    for (int k = 0; k < 40; k++) {
        g_fail_after = k;
        l = parse(s);
        g_fail_after = -1;
        if (!l) failures++;
        tl_free(l, &A);
    }
    TH_CHECK(failures > 0 && failures < 40);
    free(s);
    TH_CHECK(g_live == 0);
}

static void test_value_video_layout(void)
{
    tl_layout_t *l = parse("{\"elements\":["
        "{\"type\":\"image\",\"id\":\"vid_rpm\",\"video_playback\":\"value\",\"data_source\":\"obd.rpm\",\"range_min\":0,\"range_max\":6000,\"video_step\":100},"
        "{\"type\":\"image\",\"id\":\"vid_loop\",\"data_source\":\"obd.rpm\"},"
        "{\"type\":\"image\",\"id\":\"static\",\"video_playback\":\"value\",\"data_source\":\"obd.rpm\"},"
        "{\"type\":\"image\",\"id\":\"vid_bad\",\"video_playback\":\"value\",\"data_source\":\"obd.rpm\",\"range_min\":100,\"range_max\":10}]}");
    TH_CHECK(l && l->n == 4);
    if (l) {
        TH_CHECK(l->el[0].video_value && l->el[0].video_step == 100);
        TH_CHECK(l->el[0].range_min == 0 && l->el[0].range_max == 6000);
        TH_CHECK(l->el[0].data_source && !strcmp(l->el[0].data_source, "obd.rpm"));
        for (unsigned i = 1; i < 4; i++) TH_CHECK(!l->el[i].video_value && !l->el[i].data_source);
    }
    tl_free(l, &A);
    TH_CHECK(g_live == 0);
}

int main(void)
{
    test_round();
    test_elements();
    test_limits(); test_value_video_layout();
    return th_report("layout");
}
