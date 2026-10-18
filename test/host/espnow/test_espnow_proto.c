/* Host tests for the ESP-NOW v5/v6 wire codec (main/bsp_obd_dsp/espnow_v6.c). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "espnow_v6.h"
#include "signal_registry.h"
#include "signal_store.h"

static int s_checks;
static int s_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        s_checks++;                                                            \
        if (!(cond)) {                                                         \
            s_failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        s_checks++;                                                            \
        if (_a != _b) {                                                        \
            s_failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", \
                    __FILE__, __LINE__, #a, _a, #b, _b);                       \
        }                                                                      \
    } while (0)

_Static_assert(SIG_VEH_MAX <= 32, "veh_valid must cover every veh.* slot");
_Static_assert(SIG_OBD_COUNT <= 32, "std_valid must cover every obd.* signal");

/* ------------------------------------------------------------ helpers */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t  rdi16(const uint8_t *p) { return (int16_t)rd16(p); }

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void fix_crc(uint8_t *buf, size_t len)
{
    wr16(buf + len - 2, espnow_crc16(buf, len - 2));
}

static uint32_t fnv1a_ref(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void fill_std(espnow_snapshot_t *s)
{
    espnow_snapshot_init(s);
    s->seq = 0xDEADBEEFu;
    s->profile_hash = 0x12345678u;
    s->flags = ESPNOW_FLAG_ELM | ESPNOW_FLAG_LINKED_EN;
    s->sweep_step = 7;
    s->intro_step = 255;
    memcpy(s->name, "SkyGauge", 8);
    CHECK(espnow_snapshot_put(s, ESPNOW_ID_RPM, 3000));
    CHECK(espnow_snapshot_put(s, ESPNOW_ID_SPEED, 88));
    CHECK(espnow_snapshot_put(s, ESPNOW_ID_BOOST, -50));
    CHECK(espnow_snapshot_put(s, ESPNOW_ID_BATTERY_VOLTAGE, 12345));
    CHECK(espnow_snapshot_put(s, ESPNOW_ID_GEAR, 3));
}

/* A snapshot with every std obd.* signal plus 32 veh.* values (43 records). */
static void fill_full(espnow_snapshot_t *s)
{
    espnow_snapshot_init(s);
    s->seq = 42;
    s->profile_hash = 0xCAFEF00Du;
    for (uint16_t id = ESPNOW_ID_RPM; id <= ESPNOW_ID_LOAD; id++) {
        CHECK(espnow_snapshot_put(s, id, id == ESPNOW_ID_GEAR ? 6 : (int32_t)id * 10));
    }
    for (uint16_t i = 0; i < 32; i++) {
        CHECK(espnow_snapshot_put(s, (uint16_t)(ESPNOW_ID_VEH_FIRST + i), -1000 - i));
    }
}

/* Fake signal table: std 0x0001..0x000C -> 0..11, sys.obd_link -> 12,
 * veh 0x0100..0x0103 -> 17..20, everything else unknown. */
#define FAKE_SLOTS 24
typedef struct {
    int32_t value[FAKE_SLOTS];
    bool    valid[FAKE_SLOTS];
    int     sets[FAKE_SLOTS];
    int     invals[FAKE_SLOTS];
    int     batch_calls;
    int     inval_batch_calls;
} fake_tab_t;

static int fake_slot(void *ctx, uint16_t id)
{
    (void)ctx;
    if (id >= 0x0001 && id <= 0x000C) return id - 1;
    if (id == 0x0080) return 12;
    if (id >= 0x0100 && id <= 0x0103) return 17 + (id - 0x0100);
    return -1;
}

static void fake_set(void *ctx, int slot, int32_t v)
{
    fake_tab_t *t = ctx;
    t->value[slot] = v;
    t->valid[slot] = true;
    t->sets[slot]++;
}

static void fake_set_many(void *ctx, const int *slots, const int32_t *values, size_t count)
{
    fake_tab_t *t = ctx;
    t->batch_calls++;
    for (size_t i = 0; i < count; i++) fake_set(ctx, slots[i], values[i]);
}

static void fake_inval(void *ctx, int slot)
{
    fake_tab_t *t = ctx;
    t->valid[slot] = false;
    t->invals[slot]++;
}

static void fake_inval_many(void *ctx, const int *slots, size_t count)
{
    fake_tab_t *t = ctx;
    t->inval_batch_calls++;
    for (size_t i = 0; i < count; i++) fake_inval(ctx, slots[i]);
}

static espnow_apply_ops_t fake_ops(fake_tab_t *t)
{
    memset(t, 0, sizeof(*t));
    espnow_apply_ops_t ops = { t, fake_slot, fake_set, fake_inval, NULL, NULL };
    return ops;
}

/* -------------------------------------------------------------- tests */

static void test_crc_and_hash(void)
{
    CHECK_EQ(espnow_crc16((const uint8_t *)"123456789", 9), 0x29B1);
    CHECK_EQ(fnv1a_ref((const uint8_t *)"a", 1), 0xE40C292Cu);

    const uint8_t gen[] = { 'o', 'b', 'd', '2', '.', 'g', 'e', 'n', 'e', 'r', 'i', 'c', 0x00, 0x01, 0x00 };
    CHECK_EQ(espnow_profile_hash("obd2.generic", 1), fnv1a_ref(gen, sizeof(gen)));
    CHECK(espnow_profile_hash("obd2.generic", 1) != espnow_profile_hash("obd2.generic", 2));
    CHECK(espnow_profile_hash("toyota.gr86", 1) != espnow_profile_hash("obd2.generic", 1));
    CHECK_EQ(espnow_profile_hash(NULL, 0), espnow_profile_hash("", 0));
}

static void test_snapshot_put(void)
{
    espnow_snapshot_t s;
    int32_t v = 0;
    espnow_snapshot_init(&s);
    CHECK_EQ(s.gear, ESPNOW_GEAR_INVALID);
    CHECK(!espnow_snapshot_get(&s, ESPNOW_ID_GEAR, &v));

    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_RPM, 1000));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_RPM, 2000));           /* replace */
    CHECK_EQ(s.n_rec, 1);
    CHECK(espnow_snapshot_get(&s, ESPNOW_ID_RPM, &v));
    CHECK_EQ(v, 2000);

    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_GEAR, -1));            /* gear -> header */
    CHECK_EQ(s.n_rec, 1);
    CHECK_EQ(s.gear, -1);
    CHECK(s.std_valid & (1u << 5));
    CHECK(!espnow_snapshot_put(&s, ESPNOW_ID_GEAR, ESPNOW_GEAR_INVALID));
    CHECK(!espnow_snapshot_put(&s, ESPNOW_ID_GEAR, -200));

    CHECK(!espnow_snapshot_put(&s, 0, 1));
    CHECK(!espnow_snapshot_put(&s, 0x0080, 2));                    /* sys.obd_link */
    CHECK(!espnow_snapshot_put(&s, 0x00BF, 2));
    CHECK(espnow_snapshot_put(&s, 0x0102, 9));
    CHECK(s.veh_valid == (1u << 2));
    CHECK(espnow_snapshot_put(&s, 0x0040, 9));                     /* no mask bit */
    CHECK_EQ(s.std_valid, (1u << 0) | (1u << 5));

    espnow_snapshot_init(&s);
    for (int i = 0; i < ESPNOW_SNAP_MAX_REC; i++) {
        CHECK(espnow_snapshot_put(&s, (uint16_t)(0x0200 + i), i));
    }
    CHECK(!espnow_snapshot_put(&s, 0x0300, 1));                    /* full */
    CHECK(espnow_snapshot_put(&s, 0x0200, 77));                    /* replace still ok */
}

static void test_v6_roundtrip(void)
{
    espnow_snapshot_t s;
    fill_std(&s);
    uint8_t buf[ESPNOW_FRAME_MAX];
    CHECK_EQ(espnow_v6_frame_count(&s), 1);
    size_t len = espnow_v6_encode(&s, 0, buf, sizeof(buf));
    CHECK_EQ(len, ESPNOW_V6_FRAME_LEN(4));
    CHECK_EQ(espnow_classify(buf, len), ESPNOW_PKT_V6_DATA);

    espnow_rx_frame_t f;
    CHECK(espnow_v6_decode(buf, len, &f));
    CHECK_EQ(f.version, 6);
    CHECK_EQ(f.flags, ESPNOW_FLAG_ELM | ESPNOW_FLAG_LINKED_EN);
    CHECK_EQ(f.frag_index, 0);
    CHECK_EQ(f.frag_count, 1);
    CHECK_EQ(f.seq, 0xDEADBEEFu);
    CHECK_EQ(f.profile_hash, 0x12345678u);
    CHECK_EQ(f.std_valid, s.std_valid);
    CHECK_EQ(f.veh_valid, 0);
    CHECK_EQ(f.gear, 3);
    CHECK_EQ(f.sweep_step, 7);
    CHECK_EQ(f.intro_step, 255);
    CHECK(strcmp(f.name, "SkyGauge") == 0);
    CHECK_EQ(f.n_rec, 4);
    for (int i = 0; i < f.n_rec; i++) {
        uint16_t id;
        int32_t v;
        CHECK(espnow_rx_record(&f, i, &id, &v));
        CHECK_EQ(id, s.rec[i].num_id);
        CHECK_EQ(v, s.rec[i].value);
    }
    CHECK(!espnow_rx_record(&f, 4, &(uint16_t){0}, &(int32_t){0}));

    /* empty snapshot still produces one (heartbeat) frame */
    espnow_snapshot_init(&s);
    CHECK_EQ(espnow_v6_frame_count(&s), 1);
    len = espnow_v6_encode(&s, 0, buf, sizeof(buf));
    CHECK_EQ(len, ESPNOW_V6_FRAME_LEN(0));
    CHECK(espnow_v6_decode(buf, len, &f));
    CHECK_EQ(f.n_rec, 0);
    CHECK_EQ(f.gear, ESPNOW_GEAR_INVALID);
    CHECK_EQ(f.std_valid, 0);

    /* encoder argument checks */
    fill_std(&s);
    CHECK_EQ(espnow_v6_encode(&s, 1, buf, sizeof(buf)), 0);
    CHECK_EQ(espnow_v6_encode(&s, -1, buf, sizeof(buf)), 0);
    CHECK_EQ(espnow_v6_encode(&s, 0, buf, ESPNOW_V6_FRAME_LEN(4) - 1), 0);
}

static void test_v6_split(void)
{
    espnow_snapshot_t s;
    fill_full(&s);
    CHECK_EQ(s.n_rec, 43);                       /* gear rides in the header */
    CHECK_EQ(espnow_v6_frame_count(&s), 2);

    uint8_t b0[ESPNOW_FRAME_MAX], b1[ESPNOW_FRAME_MAX];
    size_t l0 = espnow_v6_encode(&s, 0, b0, sizeof(b0));
    size_t l1 = espnow_v6_encode(&s, 1, b1, sizeof(b1));
    CHECK_EQ(l0, ESPNOW_FRAME_MAX);               /* 35 records = exactly 250 B */
    CHECK_EQ(l1, ESPNOW_V6_FRAME_LEN(8));
    CHECK_EQ(espnow_v6_encode(&s, 2, b1, sizeof(b1)), 0);

    espnow_rx_frame_t f0, f1;
    CHECK(espnow_v6_decode(b0, l0, &f0));
    CHECK(espnow_v6_decode(b1, l1, &f1));
    CHECK_EQ(f0.frag_index, 0);
    CHECK_EQ(f1.frag_index, 1);
    CHECK_EQ(f0.frag_count, 2);
    CHECK_EQ(f1.frag_count, 2);
    CHECK_EQ(f0.seq, f1.seq);
    CHECK_EQ(f0.std_valid, 0x0FFFu);
    CHECK_EQ(f1.std_valid, 0x0FFFu);
    CHECK_EQ(f0.veh_valid, 0xFFFFFFFFu);
    CHECK_EQ(f1.veh_valid, 0xFFFFFFFFu);
    CHECK_EQ(f0.gear, 6);
    CHECK_EQ(f1.gear, 6);
    CHECK_EQ(f0.n_rec + f1.n_rec, 43);

    /* union of fragments == snapshot */
    int seen = 0;
    const espnow_rx_frame_t *fr[2] = { &f0, &f1 };
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < fr[k]->n_rec; i++) {
            uint16_t id;
            int32_t v, want;
            CHECK(espnow_rx_record(fr[k], i, &id, &v));
            CHECK(espnow_snapshot_get(&s, id, &want));
            CHECK_EQ(v, want);
            seen++;
        }
    }
    CHECK_EQ(seen, 43);

    /* full snapshot (64 records) */
    espnow_snapshot_init(&s);
    for (int i = 0; i < ESPNOW_SNAP_MAX_REC; i++) CHECK(espnow_snapshot_put(&s, (uint16_t)(0x0200 + i), i));
    CHECK_EQ(espnow_v6_frame_count(&s), 2);
    CHECK_EQ(espnow_v6_encode(&s, 1, b1, sizeof(b1)), ESPNOW_V6_FRAME_LEN(29));
    CHECK(espnow_v6_decode(b1, ESPNOW_V6_FRAME_LEN(29), &f1));
}

static void test_v6_corrupt(void)
{
    espnow_snapshot_t s;
    fill_std(&s);
    uint8_t buf[ESPNOW_FRAME_MAX + 1];
    espnow_rx_frame_t f;
    size_t len = espnow_v6_encode(&s, 0, buf, sizeof(buf));
    CHECK(espnow_v6_decode(buf, len, &f));

    for (size_t n = 0; n < len; n++) CHECK(!espnow_v6_decode(buf, n, &f));   /* truncated */
    buf[len] = 0;
    CHECK(!espnow_v6_decode(buf, len + 1, &f));                              /* trailing byte */
    CHECK(!espnow_v6_decode(NULL, len, &f));

    const uint8_t flips[] = { 0x01, 0x80, 0xFF };
    for (size_t i = 0; i < len; i++) {
        for (size_t k = 0; k < sizeof(flips); k++) {
            buf[i] ^= flips[k];
            CHECK(!espnow_v6_decode(buf, len, &f));
            buf[i] ^= flips[k];
        }
    }
    CHECK(espnow_v6_decode(buf, len, &f));

    /* structural errors with a valid CRC */
    uint8_t m[ESPNOW_FRAME_MAX];
#define MUTATE(off, val)                                                      \
    do {                                                                      \
        memcpy(m, buf, len);                                                  \
        m[off] = (uint8_t)(val);                                              \
        fix_crc(m, len);                                                      \
        CHECK(!espnow_v6_decode(m, len, &f));                                 \
    } while (0)
    MUTATE(0, 0x00);            /* magic */
    MUTATE(2, 5);               /* version */
    MUTATE(2, 7);
    MUTATE(3, ESPNOW_V6_TYPE_HELLO);
    MUTATE(3, 9);               /* type */
    MUTATE(5, 0x00);            /* frag count 0 */
    MUTATE(5, 0x22);            /* index == count */
    MUTATE(5, 0x31);            /* index > count */
    MUTATE(6, 3);               /* n_rec does not match length */
    MUTATE(6, 5);
    MUTATE(6, 36);              /* n_rec above max */
#undef MUTATE
    memcpy(m, buf, len);
    m[5] = 0xEF;                /* index 14 of 15: valid */
    fix_crc(m, len);
    CHECK(espnow_v6_decode(m, len, &f));
    CHECK_EQ(f.frag_index, 14);
    CHECK_EQ(f.frag_count, 15);

    /* gear byte is authoritative over the std_valid gear bit */
    memcpy(m, buf, len);
    m[7] = ESPNOW_GEAR_INVALID;
    fix_crc(m, len);
    CHECK(espnow_v6_decode(m, len, &f));
    CHECK(!(f.std_valid & (1u << 5)));
    memcpy(m, buf, len);
    m[16] = 0;                  /* clear std bits 0..7 incl. gear */
    fix_crc(m, len);
    CHECK(espnow_v6_decode(m, len, &f));
    CHECK(f.std_valid & (1u << 5));

    /* oversized buffer */
    uint8_t big[ESPNOW_FRAME_MAX + 6];
    memset(big, 0, sizeof(big));
    CHECK(!espnow_v6_decode(big, sizeof(big), &f));
}

static void test_v6_apply(void)
{
    fake_tab_t t;
    espnow_apply_ops_t ops = fake_ops(&t);
    espnow_snapshot_t s;
    espnow_rx_frame_t f;
    uint8_t buf[ESPNOW_FRAME_MAX];

    espnow_snapshot_init(&s);
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_RPM, 3000));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_SPEED, 88));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_GEAR, 4));
    CHECK(espnow_snapshot_put(&s, 0x000D, 5));          /* future std id, unknown here */
    CHECK(espnow_snapshot_put(&s, 0x0050, 6));          /* unknown ext id */
    CHECK(espnow_snapshot_put(&s, 0x0100, 77));
    CHECK(espnow_snapshot_put(&s, 0x0101, -5));
    CHECK(espnow_snapshot_put(&s, 0x0110, 1));          /* veh id unknown here */
    CHECK(espnow_snapshot_put(&s, 0x0400, 1));          /* beyond veh mask, unknown */
    size_t len = espnow_v6_encode(&s, 0, buf, sizeof(buf));
    /* smuggle a sys.obd_link record in (an encoder never does) */
    CHECK_EQ(rd16(buf + ESPNOW_V6_HDR_LEN + 3 * ESPNOW_V6_REC_LEN), 0x0050);
    wr16(buf + ESPNOW_V6_HDR_LEN + 3 * ESPNOW_V6_REC_LEN, 0x0080);
    fix_crc(buf, len);
    CHECK(espnow_v6_decode(buf, len, &f));
    ops.set_many = fake_set_many;
    ops.invalidate_many = fake_inval_many;

    t.valid[3] = true;   t.value[3] = 90;      /* coolant valid before: bit clear -> invalidated */
    t.valid[18] = true;  t.value[18] = 1;
    t.valid[19] = true;  t.value[19] = 123;    /* veh 0x0102: not in veh_valid */
    CHECK_EQ(espnow_rx_apply(&f, false, &ops), 3);   /* rpm, speed, gear */
    CHECK_EQ(t.batch_calls, 1);
    CHECK_EQ(t.inval_batch_calls, 1);
    CHECK(t.valid[0] && t.value[0] == 3000);
    CHECK(t.valid[1] && t.value[1] == 88);
    CHECK(t.valid[5] && t.value[5] == 4);
    CHECK(!t.valid[3]);
    CHECK_EQ(t.sets[12] + t.invals[12], 0);          /* sys never touched */
    CHECK_EQ(t.sets[17] + t.invals[17], 0);          /* veh untouched without hash match */
    CHECK_EQ(t.sets[18] + t.invals[18], 0);
    CHECK(t.valid[19]);

    CHECK_EQ(espnow_rx_apply(&f, true, &ops), 5);    /* + veh 0x0100, 0x0101 */
    CHECK_EQ(t.batch_calls, 2);
    CHECK_EQ(t.inval_batch_calls, 3); /* std + vehicle invalidation batches */
    CHECK(t.valid[17] && t.value[17] == 77);
    CHECK(t.valid[18] && t.value[18] == -5);
    CHECK(!t.valid[19]);                             /* veh bit clear -> invalidated */
    CHECK_EQ(t.sets[12] + t.invals[12], 0);

    /* gear invalid -> invalidated */
    s.gear = ESPNOW_GEAR_INVALID;
    len = espnow_v6_encode(&s, 0, buf, sizeof(buf));
    CHECK(espnow_v6_decode(buf, len, &f));
    espnow_rx_apply(&f, true, &ops);
    CHECK(!t.valid[5]);

    /* record contradicting its valid bit is ignored (and the slot invalidated) */
    espnow_snapshot_init(&s);
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_COOLANT_TEMP, 95));
    s.std_valid = 0;
    len = espnow_v6_encode(&s, 0, buf, sizeof(buf));
    CHECK(espnow_v6_decode(buf, len, &f));
    t.valid[3] = true;  t.value[3] = 1;
    CHECK_EQ(espnow_rx_apply(&f, true, &ops), 0);
    CHECK(!t.valid[3]);
    CHECK_EQ(t.value[3], 1);

    /* fragments apply independently: frag 1 does not invalidate frag-0 values */
    ops = fake_ops(&t);
    fill_full(&s);
    uint8_t b1[ESPNOW_FRAME_MAX];
    len = espnow_v6_encode(&s, 1, b1, sizeof(b1));
    CHECK(espnow_v6_decode(b1, len, &f));
    for (int i = 0; i < FAKE_SLOTS; i++) { t.valid[i] = true; t.value[i] = -1; }
    espnow_rx_apply(&f, true, &ops);
    for (int i = 0; i < 12; i++) {
        if (i == 5) continue;
        CHECK(t.valid[i]);
        CHECK_EQ(t.invals[i], 0);
        CHECK_EQ(t.value[i], -1);                    /* std records are all in frag 0 */
    }
    CHECK(t.valid[5] && t.value[5] == 6);            /* gear in every fragment */
    CHECK(t.valid[17] && t.value[17] == -1);         /* veh 0x0100..0x0103: frag 0 too */
    CHECK_EQ(t.invals[17], 0);

    /* bad ops */
    espnow_apply_ops_t bad = { &t, NULL, fake_set, fake_inval, NULL, NULL };
    CHECK_EQ(espnow_rx_apply(&f, true, &bad), 0);
}

static void test_v5(void)
{
    espnow_snapshot_t s;
    uint8_t buf[ESPNOW_V5_DATA_LEN + 1];
    espnow_rx_frame_t f;
    int32_t v = 0;

    espnow_snapshot_init(&s);
    s.seq = 77;
    s.flags = ESPNOW_FLAG_ELM | ESPNOW_FLAG_LINKTEST;
    s.sweep_step = 3;
    s.intro_step = 9;
    memcpy(s.name, "SkyGauge", 8);
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_RPM, 6500));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_SPEED, 300));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_COOLANT_TEMP, 90));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_INTAKE_TEMP, 30));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_OIL_TEMP, 105));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_OIL_PRESSURE, 345));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_BOOST, -55));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_LOAD, 40));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_THROTTLE, 12));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_BATTERY_VOLTAGE, 13800));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_AFR, 1470));
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_GEAR, 2));
    CHECK(espnow_snapshot_put(&s, 0x0100, 5));

    CHECK_EQ(espnow_v5_encode(&s, buf, ESPNOW_V5_DATA_LEN - 1), 0);
    size_t len = espnow_v5_encode(&s, buf, sizeof(buf));
    CHECK_EQ(len, 47);
    CHECK_EQ(espnow_classify(buf, len), ESPNOW_PKT_V5_DATA);
    CHECK_EQ(rd16(buf), 0x4F42);
    CHECK_EQ(buf[2], 5);
    CHECK_EQ(rdi16(buf + ESPNOW_V5_RETIRED_OFS), -1000);     /* retired slot */
    CHECK_EQ(rd16(buf + 8), 6500);
    CHECK_EQ(buf[10], 255);                                   /* speed clamped */
    CHECK_EQ(rdi16(buf + 19), 35);                            /* 3.45 bar -> 35 (x10) */
    CHECK_EQ(rdi16(buf + 21), -6);                            /* -0.55 bar -> -6 */
    CHECK(memcmp(buf + 35, "SkyGauge\0\0\0\0", 12) == 0);

    CHECK(espnow_v5_decode(buf, len, &f));
    CHECK_EQ(f.version, 5);
    CHECK_EQ(f.flags, ESPNOW_FLAG_ELM | ESPNOW_FLAG_LINKTEST);
    CHECK_EQ(f.seq, 77);
    CHECK_EQ(f.sweep_step, 3);
    CHECK_EQ(f.intro_step, 9);
    CHECK_EQ(f.gear, ESPNOW_GEAR_INVALID);                    /* v5 has no gear */
    CHECK_EQ(f.profile_hash, 0);
    CHECK_EQ(f.veh_valid, 0);
    CHECK_EQ(f.n_rec, 11);
    CHECK_EQ(f.std_valid, 0x0FFFu & ~(1u << 5));
    CHECK(strcmp(f.name, "SkyGauge") == 0);

    fake_tab_t t;
    espnow_apply_ops_t ops = fake_ops(&t);
    t.valid[5] = true;
    t.valid[17] = true;
    CHECK_EQ(espnow_rx_apply(&f, false, &ops), 11);
    CHECK_EQ(t.value[0], 6500);
    CHECK_EQ(t.value[1], 255);
    CHECK_EQ(t.value[2], -60);                                /* x10 -> 0.01 bar */
    CHECK_EQ(t.value[3], 90);
    CHECK_EQ(t.value[4], 350);
    CHECK_EQ(t.value[6], 13800);
    CHECK_EQ(t.value[7], 105);
    CHECK_EQ(t.value[8], 1470);
    CHECK_EQ(t.value[9], 12);
    CHECK_EQ(t.value[10], 30);
    CHECK_EQ(t.value[11], 40);
    CHECK(!t.valid[5]);                                       /* gear invalidated */
    CHECK(t.valid[17]);                                       /* veh untouched */

    /* empty snapshot -> legacy sentinels -> only rpm/speed valid on decode */
    espnow_snapshot_init(&s);
    len = espnow_v5_encode(&s, buf, sizeof(buf));
    CHECK_EQ(rdi16(buf + 13), -40);
    CHECK_EQ(rdi16(buf + 15), -40);
    CHECK_EQ(rdi16(buf + 17), -100);
    CHECK_EQ(rdi16(buf + 19), -1);
    CHECK_EQ(rdi16(buf + 21), -32768);
    CHECK_EQ(rdi16(buf + 23), -1000);
    CHECK_EQ(rdi16(buf + 25), -1);
    CHECK_EQ(rdi16(buf + 27), -1);
    CHECK_EQ((int32_t)(rd16(buf + 29) | ((uint32_t)rd16(buf + 31) << 16)), -1);
    CHECK_EQ(rdi16(buf + 33), -1);
    CHECK(espnow_v5_decode(buf, len, &f));
    CHECK_EQ(f.n_rec, 2);
    CHECK_EQ(f.std_valid, 0x3u);
    ops = fake_ops(&t);
    for (int i = 0; i < 12; i++) t.valid[i] = true;
    CHECK_EQ(espnow_rx_apply(&f, false, &ops), 2);
    for (int i = 2; i < 12; i++) CHECK(!t.valid[i]);

    /* hand-built legacy packet: oil temp outside [-20,150] holds, -100 invalidates */
    espnow_snapshot_init(&s);
    CHECK(espnow_snapshot_put(&s, ESPNOW_ID_OIL_TEMP, 100));
    len = espnow_v5_encode(&s, buf, sizeof(buf));
    wr16(buf + 17, 200);
    CHECK(espnow_v5_decode(buf, len, &f));
    CHECK(f.std_valid & (1u << 7));
    CHECK(!espnow_snapshot_get(&s, 0x9999, &v));
    ops = fake_ops(&t);
    t.valid[7] = true;  t.value[7] = 88;
    espnow_rx_apply(&f, false, &ops);
    CHECK(t.valid[7] && t.value[7] == 88 && t.sets[7] == 0 && t.invals[7] == 0);
    wr16(buf + 17, (uint16_t)-100);
    CHECK(espnow_v5_decode(buf, len, &f));
    espnow_rx_apply(&f, false, &ops);
    CHECK(!t.valid[7]);
    wr16(buf + 17, (uint16_t)-20);
    CHECK(espnow_v5_decode(buf, len, &f));
    espnow_rx_apply(&f, false, &ops);
    CHECK(t.valid[7] && t.value[7] == -20);

    /* retired offset 23 is never read */
    wr16(buf + 23, 1234);
    espnow_rx_frame_t f2;
    CHECK(espnow_v5_decode(buf, len, &f2));
    CHECK_EQ(f2.n_rec, f.n_rec);
    CHECK_EQ(f2.std_valid, f.std_valid);

    /* rejects */
    CHECK(!espnow_v5_decode(buf, len - 1, &f));
    CHECK(!espnow_v5_decode(buf, len + 1, &f));
    buf[2] = 6;
    CHECK(!espnow_v5_decode(buf, len, &f));
    buf[2] = 5;
    buf[0] ^= 1;
    CHECK(!espnow_v5_decode(buf, len, &f));
}

static void test_classify_hello(void)
{
    uint8_t buf[ESPNOW_FRAME_MAX];
    uint8_t pos = 0, caps = 0;

    CHECK_EQ(espnow_hello_encode(2, ESPNOW_CAP_V6_DATA, buf, 7), 0);
    size_t len = espnow_hello_encode(2, ESPNOW_CAP_V6_DATA, buf, sizeof(buf));
    CHECK_EQ(len, ESPNOW_V6_HELLO_LEN);
    CHECK_EQ(espnow_classify(buf, len), ESPNOW_PKT_V6_HELLO);
    CHECK(espnow_hello_decode(buf, len, &pos, &caps));
    CHECK_EQ(pos, 2);
    CHECK_EQ(caps, ESPNOW_CAP_V6_DATA);
    CHECK(!espnow_hello_decode(buf, len - 1, &pos, &caps));
    CHECK(espnow_hello_decode(buf, len + 4, &pos, &caps));   /* appended fields tolerated */
    CHECK(!espnow_v6_decode(buf, len, &(espnow_rx_frame_t){0}));
    buf[3] = ESPNOW_V6_TYPE_DATA;
    CHECK(!espnow_hello_decode(buf, len, &pos, &caps));

    const uint8_t presence[4] = { 0x42, 0x4F, 5, 1 };
    const uint8_t ctrl[6] = { 0x42, 0x4F, 5, 1, 0, 0 };
    CHECK_EQ(espnow_classify(presence, 4), ESPNOW_PKT_V5_PRESENCE);
    CHECK_EQ(espnow_classify(ctrl, 6), ESPNOW_PKT_V5_CTRL);
    CHECK_EQ(espnow_classify(ctrl, 5), ESPNOW_PKT_UNKNOWN);
    CHECK_EQ(espnow_classify(presence, 2), ESPNOW_PKT_UNKNOWN);
    CHECK_EQ(espnow_classify(NULL, 6), ESPNOW_PKT_UNKNOWN);
    const uint8_t badmagic[4] = { 0x43, 0x4F, 5, 1 };
    CHECK_EQ(espnow_classify(badmagic, 4), ESPNOW_PKT_UNKNOWN);
    const uint8_t v7[8] = { 0x42, 0x4F, 7, 1, 0, 0, 0, 0 };
    CHECK_EQ(espnow_classify(v7, 8), ESPNOW_PKT_UNKNOWN);

    /* no v6 DATA length collides with the lengths old firmware dispatches on */
    for (int n = 0; n <= ESPNOW_V6_MAX_REC; n++) {
        int l = ESPNOW_V6_FRAME_LEN(n);
        CHECK(l != ESPNOW_V5_DATA_LEN && l != ESPNOW_V5_PRESENCE_LEN && l != ESPNOW_V5_CTRL_LEN);
        CHECK(l <= ESPNOW_FRAME_MAX);
    }
    CHECK(ESPNOW_V6_HELLO_LEN != ESPNOW_V5_DATA_LEN && ESPNOW_V6_HELLO_LEN != ESPNOW_V5_PRESENCE_LEN &&
          ESPNOW_V6_HELLO_LEN != ESPNOW_V5_CTRL_LEN);
}

static void test_registry_ids(void)
{
    static const struct { const char *key; uint16_t id; } want[] = {
        { "obd.rpm", ESPNOW_ID_RPM },
        { "obd.speed", ESPNOW_ID_SPEED },
        { "obd.boost", ESPNOW_ID_BOOST },
        { "obd.coolant_temp", ESPNOW_ID_COOLANT_TEMP },
        { "obd.oil_pressure", ESPNOW_ID_OIL_PRESSURE },
        { "obd.gear", ESPNOW_ID_GEAR },
        { "obd.battery_voltage", ESPNOW_ID_BATTERY_VOLTAGE },
        { "obd.oil_temp", ESPNOW_ID_OIL_TEMP },
        { "obd.afr", ESPNOW_ID_AFR },
        { "obd.throttle", ESPNOW_ID_THROTTLE },
        { "obd.intake_temp", ESPNOW_ID_INTAKE_TEMP },
        { "obd.load", ESPNOW_ID_LOAD },
    };
    CHECK_EQ(sizeof(want) / sizeof(want[0]), SIG_OBD_COUNT);
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int found = 0;
        for (int k = 0; k < SIG_STD_COUNT; k++) {
            if (strcmp(sig_registry[k].key, want[i].key) == 0) {
                CHECK_EQ(sig_registry[k].num_id, want[i].id);
                found = 1;
            }
        }
        CHECK(found);
    }
    for (int k = 0; k < SIG_STD_COUNT; k++) {
        uint16_t id = sig_registry[k].num_id;
        if (strncmp(sig_registry[k].key, "sys.", 4) == 0) {
            CHECK(id >= ESPNOW_ID_SYS_FIRST && id <= ESPNOW_ID_SYS_LAST);
        } else {
            CHECK(id >= ESPNOW_ID_STD_FIRST && id < ESPNOW_ID_STD_FIRST + 32);
        }
    }
    CHECK_EQ(SIG_VEH_ID_BASE, ESPNOW_ID_VEH_FIRST);
}

int main(void)
{
    test_crc_and_hash();
    test_snapshot_put();
    test_v6_roundtrip();
    test_v6_split();
    test_v6_corrupt();
    test_v6_apply();
    test_v5();
    test_classify_hello();
    test_registry_ids();
    printf("espnow_proto: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
