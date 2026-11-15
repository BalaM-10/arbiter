/*
 * Unit tests for libcontract. Built and run on the host (with sanitizers) and
 * on QNX (via scripts/run-tests.sh). Each test name starts with the ID of the
 * requirement it covers (docs/REQUIREMENTS_SUMMARY.md, docs/TRACEABILITY.md).
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arb_arm_model.h"
#include "arb_contract.h"
#include "arb_test.h"

#define MS(x) ((uint64_t)(x) * 1000000ull)
#define T0    MS(1000)  /* arbitrary start time */

/* ------------------------------------------------------------------ helpers */

static void seal(arb_contract_table_t *t)
{
    t->crc32 = arb_crc32(t, offsetof(arb_contract_table_t, crc32));
}

/* A valid contract. with_zone = 0 gives one with no keep-out zones. */
static arb_contract_table_t make_table(int with_zone)
{
    arb_contract_table_t t;

    memset(&t, 0, sizeof t);
    t.magic = ARB_CONTRACT_MAGIC;
    t.version = ARB_CONTRACT_VERSION;
    t.table_size = (uint16_t)sizeof t;
    for (int i = 0; i < 32; i++) {
        t.source_sha256[i] = (uint8_t)(0xA0 + i);
    }
    t.joint_min_urad[0] = -2600000;
    t.joint_max_urad[0] = 2600000;
    t.joint_min_urad[1] = -2400000;
    t.joint_max_urad[1] = 2400000;
    t.vmax_urad_s = 1000000;          /* 1 rad/s */
    t.silence_timeout_ms = 250;
    t.rate_per_s = 50;
    t.rate_burst = 10;
    t.violation_threshold = 3;
    t.violation_window_ms = 1000;
    t.max_duration_ms = 10000;        /* a full-range move needs 5200 ms at vmax */
    if (with_zone) {
        t.zone_count = 1;
        t.zones[0].x_min_um = 200000;   /* operator's hand area: x 0.20..0.40 m, y 0.15..0.35 m */
        t.zones[0].y_min_um = 150000;
        t.zones[0].x_max_um = 400000;
        t.zones[0].y_max_um = 350000;
    }
    seal(&t);
    return t;
}

static arb_cmd_t make_cmd(uint32_t seq, int32_t q0, int32_t q1, uint32_t duration_ms)
{
    arb_cmd_t c;

    memset(&c, 0, sizeof c);
    c.version = ARB_PROTO_VERSION;
    c.seq = seq;
    c.sender_ts_ns = 123456789ull;
    c.target_urad[0] = q0;
    c.target_urad[1] = q1;
    c.duration_ms = duration_ms;
    return c;
}

static const int32_t HOME[2] = { 0, 0 };

/* Decide one command against a fresh guard on table t. */
static arb_verdict_t decide_once(const arb_contract_table_t *t, const arb_cmd_t *c, const int32_t cur[2])
{
    arb_guard_t g;
    arb_verdict_t v;

    arb_guard_init(&g, t, T0);
    arb_guard_check(&g, c, sizeof *c, cur, T0, &v);
    return v;
}

static int is_valid_verdict(const arb_verdict_t *v)
{
    return v->verdict == ARB_VERDICT_ALLOW || v->verdict == ARB_VERDICT_CLAMP ||
           v->verdict == ARB_VERDICT_DENY;
}

/* ------------------------------------------------------------ FR-C4 / FR-C5 */

static void fr_c4_crc32_matches_zlib_check_value(void)
{
    /* Standard CRC-32/IEEE check value, identical to Python's zlib.crc32(b"123456789"). */
    CHECK_EQ_INT(arb_crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ_INT(arb_crc32("", 0), 0u);
}

static void fr_c4_valid_table_loads(void)
{
    arb_contract_table_t t = make_table(1), out;

    memset(&out, 0, sizeof out);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_OK);
    CHECK(memcmp(&t, &out, sizeof t) == 0);
}

static void fr_c4_null_arguments_rejected(void)
{
    arb_contract_table_t t = make_table(1), out;

    CHECK_EQ_INT(arb_contract_load(NULL, sizeof t, &out), ARB_LOAD_E_ARG);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, NULL), ARB_LOAD_E_ARG);
}

static void fr_c4_wrong_length_rejected(void)
{
    uint8_t buf[sizeof(arb_contract_table_t) + 1];
    arb_contract_table_t t = make_table(1), out;

    memset(buf, 0, sizeof buf);
    memcpy(buf, &t, sizeof t);
    CHECK_EQ_INT(arb_contract_load(buf, 0, &out), ARB_LOAD_E_SIZE);
    CHECK_EQ_INT(arb_contract_load(buf, sizeof t - 1, &out), ARB_LOAD_E_SIZE);
    CHECK_EQ_INT(arb_contract_load(buf, sizeof t + 1, &out), ARB_LOAD_E_SIZE);
}

static void fr_c4_bad_magic_rejected(void)
{
    arb_contract_table_t t = make_table(1), out;

    t.magic ^= 1u;
    seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_E_MAGIC);
}

static void fr_c4_version_mismatch_rejected(void)
{
    arb_contract_table_t t = make_table(1), out;

    t.version = ARB_CONTRACT_VERSION + 1u;
    seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_E_VERSION);
}

static void fr_c4_table_size_field_mismatch_rejected(void)
{
    arb_contract_table_t t = make_table(1), out;

    t.table_size = (uint16_t)(sizeof t - 4u);
    seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_E_TABLESIZE);
}

static void fr_c4_crc_mismatch_rejected(void)
{
    arb_contract_table_t t = make_table(1), out;

    t.crc32 ^= 0x00010000u;
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_E_CRC);
}

static void fr_c4_any_single_byte_change_rejected(void)
{
    const arb_contract_table_t t = make_table(1);
    uint8_t buf[sizeof t];
    arb_contract_table_t out;

    /* Flip each byte in turn (including the CRC itself): never accepted. */
    for (size_t i = 0; i < sizeof t; i++) {
        memcpy(buf, &t, sizeof t);
        buf[i] ^= 0x5Au;
        if (arb_contract_load(buf, sizeof buf, &out) == ARB_LOAD_OK) {
            printf("  byte %zu flipped and still accepted\n", i);
            CHECK(0);
        }
    }
}

static void fr_c4_out_of_range_values_rejected(void)
{
    arb_contract_table_t out;
    arb_contract_table_t t;

#define EXPECT_RANGE_REJECT(mutation)                                     \
    do {                                                                  \
        t = make_table(1);                                                \
        mutation;                                                         \
        seal(&t);                                                         \
        CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_E_RANGE); \
    } while (0)

    EXPECT_RANGE_REJECT(t.joint_min_urad[0] = t.joint_max_urad[0]);           /* min == max */
    EXPECT_RANGE_REJECT(t.joint_min_urad[1] = t.joint_max_urad[1] + 1);       /* min > max */
    EXPECT_RANGE_REJECT(t.joint_max_urad[0] = ARB_ANGLE_LIMIT_URAD + 1);      /* beyond pi */
    EXPECT_RANGE_REJECT(t.joint_min_urad[1] = -ARB_ANGLE_LIMIT_URAD - 1);
    EXPECT_RANGE_REJECT(t.vmax_urad_s = 0);
    EXPECT_RANGE_REJECT(t.vmax_urad_s = ARB_VMAX_MAX_URAD_S + 1u);
    EXPECT_RANGE_REJECT(t.silence_timeout_ms = ARB_SILENCE_MIN_MS - 1u);
    EXPECT_RANGE_REJECT(t.rate_per_s = 0);
    EXPECT_RANGE_REJECT(t.rate_burst = ARB_BURST_MAX + 1u);
    EXPECT_RANGE_REJECT(t.violation_threshold = 0);
    EXPECT_RANGE_REJECT(t.violation_threshold = ARB_MAX_VIOLATION_THRESHOLD + 1u);
    EXPECT_RANGE_REJECT(t.violation_window_ms = 0);
    EXPECT_RANGE_REJECT(t.zone_count = ARB_MAX_ZONES + 1);
    EXPECT_RANGE_REJECT(t.zones[0].x_min_um = t.zones[0].x_max_um);           /* empty zone */
    EXPECT_RANGE_REJECT(t.zones[0].y_max_um = ARB_WORKSPACE_LIMIT_UM + 1);
    EXPECT_RANGE_REJECT(t.zones[2].x_max_um = 5);                             /* unused slot not zero */
#undef EXPECT_RANGE_REJECT

    /* Boundaries that must still load. */
    t = make_table(1);
    t.joint_min_urad[0] = -ARB_ANGLE_LIMIT_URAD;             /* full +-pi: 6284 ms at 1 rad/s */
    t.joint_max_urad[0] = ARB_ANGLE_LIMIT_URAD;
    t.violation_threshold = ARB_MAX_VIOLATION_THRESHOLD;
    seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_OK);
    t = make_table(1);                                       /* slowest vmax: needs a small range */
    t.vmax_urad_s = ARB_VMAX_MIN_URAD_S;
    t.joint_min_urad[0] = -25000; t.joint_max_urad[0] = 25000;   /* 50 mrad: 50 s at 1 mrad/s */
    t.joint_min_urad[1] = -25000; t.joint_max_urad[1] = 25000;
    t.max_duration_ms = ARB_DURATION_CEIL_MS;
    seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_OK);
    t = make_table(0);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_OK);           /* no zones is valid */
}

static void fr_c4_max_duration_must_cover_a_full_range_move(void)
{
    arb_contract_table_t t, out;

    /* Joint 1 spans 5.2 rad; at 1 rad/s a full-range move takes 5200 ms. */
    t = make_table(1); t.max_duration_ms = 5200; seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_OK);
    t = make_table(1); t.max_duration_ms = 5199; seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_E_RANGE);
    t = make_table(1); t.max_duration_ms = 0; seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_E_RANGE);
    t = make_table(1); t.max_duration_ms = ARB_DURATION_CEIL_MS; seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_OK);
    t = make_table(1); t.max_duration_ms = ARB_DURATION_CEIL_MS + 1u; seal(&t);
    CHECK_EQ_INT(arb_contract_load(&t, sizeof t, &out), ARB_LOAD_E_RANGE);
}

static void fr_c4_output_untouched_on_error(void)
{
    arb_contract_table_t t = make_table(1), out;

    memset(&out, 0x77, sizeof out);
    t.crc32 ^= 1u;
    CHECK(arb_contract_load(&t, sizeof t, &out) != ARB_LOAD_OK);
    for (size_t i = 0; i < sizeof out; i++) {
        if (((const uint8_t *)&out)[i] != 0x77u) {
            CHECK(0);
            break;
        }
    }
}

static void fr_c5_text_contract_rejected(void)
{
    /* The guardian never parses text: a YAML file handed to the loader is refused. */
    static const char yaml[] =
        "version: 1\njoints:\n  - {min_deg: -150, max_deg: 150}\n  - {min_deg: -135, max_deg: 135}\n";
    uint8_t buf[sizeof(arb_contract_table_t)];
    arb_contract_table_t out;

    CHECK(arb_contract_load(yaml, sizeof yaml - 1, &out) != ARB_LOAD_OK);
    memset(buf, ' ', sizeof buf);
    memcpy(buf, yaml, sizeof yaml - 1);   /* same text, padded to the exact table size */
    CHECK(arb_contract_load(buf, sizeof buf, &out) != ARB_LOAD_OK);
}

/* ------------------------------------------------------------ NFR-I6 / FR-G2 */

static void nfr_i6_command_wrong_length_rejected(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_cmd_t c = make_cmd(1, 0, 0, 100);
    uint8_t big[4096];
    arb_guard_t g;
    arb_verdict_t v;

    memset(big, 0, sizeof big);
    memcpy(big, &c, sizeof c);
    arb_guard_init(&g, &t, T0);
    arb_guard_check(&g, NULL, sizeof c, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_MALFORMED);
    arb_guard_check(&g, &c, 0, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_MALFORMED);
    arb_guard_check(&g, &c, sizeof c - 1, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_MALFORMED);
    arb_guard_check(&g, big, sizeof c + 1, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_MALFORMED);
    arb_guard_check(&g, big, sizeof big, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_MALFORMED);
    CHECK_EQ_INT(g.have_seq, 0);                   /* nothing malformed touches state */
}

static void nfr_i6_command_bad_fields_rejected(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_cmd_t c;
    arb_verdict_t v;

    c = make_cmd(1, 0, 0, 100); c.version = ARB_PROTO_VERSION + 1u;
    v = decide_once(&t, &c, HOME); CHECK(v.rule == ARB_RULE_MALFORMED);
    c = make_cmd(1, 0, 0, 100); c.flags = 1u;
    v = decide_once(&t, &c, HOME); CHECK(v.rule == ARB_RULE_MALFORMED);
    c = make_cmd(1, 0, 0, 100); c.reserved = 1u;
    v = decide_once(&t, &c, HOME); CHECK(v.rule == ARB_RULE_MALFORMED);
    c = make_cmd(1, 0, 0, 100);                          /* the same command, well-formed */
    v = decide_once(&t, &c, HOME); CHECK(v.verdict == ARB_VERDICT_ALLOW);
}

/* ------------------------------------------------- FR-G2: duration bounds */

static void fr_g2_duration_zero_denied(void)
{
    const arb_contract_table_t t = make_table(0);
    const arb_cmd_t c = make_cmd(1, 0, 0, 0);
    const arb_verdict_t v = decide_once(&t, &c, HOME);

    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_DURATION);
}

static void fr_g2_duration_minimum_and_contract_max_allowed(void)
{
    const arb_contract_table_t t = make_table(0);        /* max_duration_ms = 10000 */
    arb_cmd_t c;
    arb_verdict_t v;

    c = make_cmd(1, 0, 0, ARB_DURATION_MIN_MS);          /* exactly 1 ms */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_ALLOW && v.duration_ms == 1u);
    c = make_cmd(1, 0, 0, 10000);                        /* exactly at the contract max */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_ALLOW && v.duration_ms == 10000u);
    c = make_cmd(1, 0, 0, 9999);                         /* just inside */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
}

static void fr_g2_duration_one_over_contract_max_denied(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_cmd_t c;
    arb_verdict_t v;

    c = make_cmd(1, 0, 0, 10001);
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_DURATION);
    c = make_cmd(1, 0, 0, UINT32_MAX);
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_DURATION);
}

static void fr_g2_clamp_beyond_contract_max_denied(void)
{
    /* With max_duration_ms exactly a full-range move, a CLAMP can only exceed
     * it if the arm reports a position outside its joint limits: fail closed. */
    arb_contract_table_t t = make_table(0);
    const int32_t outside[2] = { -3000000, 0 };          /* sane angle, beyond joint 1's -2.6 rad limit */
    arb_cmd_t c;
    arb_verdict_t v;

    t.max_duration_ms = 5200;
    seal(&t);
    c = make_cmd(1, 2600000, 0, 100);                    /* needs 5600 ms at 1 rad/s */
    v = decide_once(&t, &c, outside);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_DURATION);
    c = make_cmd(1, 2600000, 0, 100);                    /* from home: needs 2600 ms, a normal CLAMP */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_CLAMP && v.duration_ms == 2600u);
}

static uint64_t xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static void nfr_i6_random_bytes_never_crash_or_allow_garbage(void)
{
    const arb_contract_table_t t = make_table(1);
    uint8_t buf[64];
    uint64_t seed = 0x9E3779B97F4A7C15ull;   /* fixed: the test is reproducible */
    arb_guard_t g;
    arb_verdict_t v;
    int accepted = 0;

    arb_guard_init(&g, &t, T0);
    for (int i = 0; i < 20000; i++) {
        const size_t len = (size_t)(xorshift64(&seed) % (sizeof buf + 1u));
        for (size_t k = 0; k < sizeof buf; k++) {
            buf[k] = (uint8_t)xorshift64(&seed);
        }
        arb_guard_check(&g, buf, len, HOME, T0 + (uint64_t)i * 1000u, &v);
        CHECK(is_valid_verdict(&v));
        if (len != sizeof(arb_cmd_t)) {
            CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_MALFORMED);
        }
        if (v.verdict != ARB_VERDICT_DENY) {
            accepted++;
        }
    }
    /* Random bytes essentially never form a valid version-1 command. */
    CHECK_EQ_INT(accepted, 0);
}

static void fr_g2_command_fields_carried_through(void)
{
    const arb_contract_table_t t = make_table(0);
    const arb_cmd_t c = make_cmd(42, 100000, -200000, 900);
    const arb_verdict_t v = decide_once(&t, &c, HOME);

    CHECK(v.verdict == ARB_VERDICT_ALLOW);
    CHECK_EQ_INT(v.seq, 42);
    CHECK_EQ_INT(v.target_urad[0], 100000);
    CHECK_EQ_INT(v.target_urad[1], -200000);
    CHECK_EQ_INT(v.requested_ms, 900);
    CHECK_EQ_INT(v.duration_ms, 900);
}

/* -------------------------------------------------------------------- FR-G3 */

static void fr_g3_every_verdict_names_its_rule(void)
{
    const arb_contract_table_t t = make_table(1);
    arb_cmd_t c;
    arb_verdict_t v;

    c = make_cmd(1, -100000, 0, 1000);                 /* small move away from the zone */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_ALLOW && v.rule == ARB_RULE_NONE);
    c = make_cmd(1, -500000, 0, 100);                  /* too fast: needs 500 ms */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_CLAMP && v.rule == ARB_RULE_VELOCITY);
    c = make_cmd(1, 2600001, 0, 5000);
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_JOINT_LIMIT);
    CHECK_EQ_INT(v.target_urad[0], 0);                 /* a DENY never carries a setpoint */
    CHECK_EQ_INT(v.duration_ms, 0);
}

/* -------------------------------------------------------- FR-G4: sequence */

static void fr_g4_sequence_replay_and_old_denied(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;

    arb_guard_init(&g, &t, T0);
    c = make_cmd(10, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(1), &v);          /* exact replay */
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_SEQUENCE);
    c = make_cmd(9, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(2), &v);          /* older */
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_SEQUENCE);
    c = make_cmd(11, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(3), &v);          /* next */
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
    c = make_cmd(1000, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(4), &v);          /* gaps are fine */
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
}

static void fr_g4_sequence_wraps_around(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;

    arb_guard_init(&g, &t, T0);
    c = make_cmd(0xFFFFFFF0u, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
    c = make_cmd(5u, 0, 0, 10);                                       /* newer across the wrap */
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(1), &v);
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
    c = make_cmd(0xFFFFFFF8u, 0, 0, 10);                              /* older across the wrap */
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(2), &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_SEQUENCE);
    c = make_cmd(5u + 0x80000000u, 0, 0, 10);                         /* exactly half the space: not newer */
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(3), &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_SEQUENCE);
}

/* ------------------------------------------------------------ FR-G4: rate */

static void fr_g4_rate_limit_burst_then_deny_then_refill(void)
{
    const arb_contract_table_t t = make_table(0);   /* 50/s, burst 10 */
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;
    uint32_t seq = 1;

    arb_guard_init(&g, &t, T0);
    for (int i = 0; i < 10; i++) {                  /* the whole burst at one instant */
        c = make_cmd(seq++, 0, 0, 10);
        arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
        CHECK(v.verdict == ARB_VERDICT_ALLOW);
    }
    c = make_cmd(seq++, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_RATE);
    c = make_cmd(seq++, 0, 0, 10);                  /* 19 ms later: less than one token (20 ms) */
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(19), &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_RATE);
    c = make_cmd(seq++, 0, 0, 10);                  /* 20 ms after T0: exactly one token */
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(20), &v);
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
    c = make_cmd(seq++, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(20), &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_RATE);
}

static void fr_g4_rate_limit_uses_guardian_clock_not_sender_clock(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;

    arb_guard_init(&g, &t, T0);
    for (uint32_t i = 1; i <= 11; i++) {
        c = make_cmd(i, 0, 0, 10);
        c.sender_ts_ns = (uint64_t)i * 1000000000000ull;  /* sender claims hours apart */
        arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    }
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_RATE);
}

static void fr_g4_rate_limit_sustained_rate_holds(void)
{
    const arb_contract_table_t t = make_table(0);   /* 50/s: one token per 20 ms */
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;
    int allowed = 0;

    arb_guard_init(&g, &t, T0);
    /* 10 s of commands every 1 ms: burst 10 + 10 s * 50/s = 510 allowed at most. */
    for (uint32_t i = 0; i < 10000; i++) {
        c = make_cmd(i + 1u, 0, 0, 10);
        arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(i), &v);
        if (v.verdict == ARB_VERDICT_ALLOW) {
            allowed++;
        }
    }
    CHECK(allowed >= 505 && allowed <= 510);
}

/* ------------------------------------------------------ FR-G4: joint limits */

static void fr_g4_joint_limit_exactly_at_max(void)
{
    const arb_contract_table_t t = make_table(0);
    const int32_t cur[2] = { 2500000, 2300000 };
    arb_cmd_t c;

    c = make_cmd(1, 2600000, 0, 5000);  CHECK(decide_once(&t, &c, cur).verdict == ARB_VERDICT_ALLOW);
    c = make_cmd(1, 0, 2400000, 5000);  CHECK(decide_once(&t, &c, cur).verdict == ARB_VERDICT_ALLOW);
}

static void fr_g4_joint_limit_just_inside(void)
{
    const arb_contract_table_t t = make_table(0);
    const int32_t cur[2] = { 0, 0 };
    arb_cmd_t c;

    c = make_cmd(1, 2599999, 0, 5000);   CHECK(decide_once(&t, &c, cur).verdict == ARB_VERDICT_ALLOW);
    c = make_cmd(1, -2599999, 0, 5000);  CHECK(decide_once(&t, &c, cur).verdict == ARB_VERDICT_ALLOW);
    c = make_cmd(1, 0, 2399999, 5000);   CHECK(decide_once(&t, &c, cur).verdict == ARB_VERDICT_ALLOW);
    c = make_cmd(1, 0, -2399999, 5000);  CHECK(decide_once(&t, &c, cur).verdict == ARB_VERDICT_ALLOW);
}

static void fr_g4_joint_limit_just_outside_denied(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_verdict_t v;
    arb_cmd_t c;

    c = make_cmd(1, 2600001, 0, 5000);  v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_JOINT_LIMIT);
    c = make_cmd(1, -2600001, 0, 5000); v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_JOINT_LIMIT);
    c = make_cmd(1, 0, 2400001, 5000);  v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_JOINT_LIMIT);
    c = make_cmd(1, 0, -2400001, 5000); v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_JOINT_LIMIT);
    c = make_cmd(1, INT32_MAX, INT32_MIN, 5000); v = decide_once(&t, &c, HOME);  /* extremes: no overflow */
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_JOINT_LIMIT);
}

static void fr_g4_impossible_arm_position_fails_closed(void)
{
    const arb_contract_table_t t = make_table(0);
    const int32_t bad[2] = { ARB_ANGLE_LIMIT_URAD + 1, 0 };
    const arb_cmd_t c = make_cmd(1, 0, 0, 5000);
    arb_guard_t g;
    arb_verdict_t v;

    v = decide_once(&t, &c, bad);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_INTERNAL);
    arb_guard_init(&g, &t, T0);
    arb_guard_check(&g, &c, sizeof c, NULL, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_INTERNAL);
}

static void fr_g4_unusable_contract_fails_closed(void)
{
    /* A guard handed a table that never went through arb_contract_load(), with
     * zero velocity and rate limits, must deny instead of dividing by zero. */
    arb_contract_table_t t;
    const arb_cmd_t c = make_cmd(1, 0, 0, 10);
    arb_guard_t g;
    arb_verdict_t v;

    memset(&t, 0, sizeof t);
    arb_guard_init(&g, &t, T0);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_INTERNAL);
    CHECK_EQ_INT(arb_guard_tick(&g, T0 + MS(10000)), 0);
}

/* ---------------------------------------------------------- FR-G4: velocity */

static void fr_g4_velocity_exactly_at_limit_allowed(void)
{
    const arb_contract_table_t t = make_table(0);         /* vmax 1 rad/s */
    const arb_cmd_t c = make_cmd(1, 500000, 0, 500);       /* 0.5 rad in 500 ms = 1 rad/s */
    const arb_verdict_t v = decide_once(&t, &c, HOME);

    CHECK(v.verdict == ARB_VERDICT_ALLOW);
    CHECK_EQ_INT(v.duration_ms, 500);
}

static void fr_g4_velocity_over_limit_clamped_by_stretching_duration(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_verdict_t v;
    arb_cmd_t c;

    c = make_cmd(1, 500000, 0, 499);                       /* 1 ms too short */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_CLAMP && v.rule == ARB_RULE_VELOCITY);
    CHECK_EQ_INT(v.duration_ms, 500);
    CHECK_EQ_INT(v.requested_ms, 499);
    CHECK_EQ_INT(v.target_urad[0], 500000);               /* target kept, only time stretched */

    c = make_cmd(1, 500001, 0, 1);                         /* rounds up: 500.001 ms -> 501 */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_CLAMP);
    CHECK_EQ_INT(v.duration_ms, 501);

    c = make_cmd(1, 0, -300000, 1);                        /* the faster joint decides */
    v = decide_once(&t, &c, HOME);
    CHECK_EQ_INT(v.duration_ms, 300);
}

static void fr_g4_velocity_clamped_motion_never_exceeds_vmax(void)
{
    const arb_contract_table_t t = make_table(0);
    uint64_t seed = 12345u;

    /* Random targets and durations: the forwarded duration always keeps every
     * joint at or under vmax (checked here independently of the library, in
     * exact integers: |delta| [urad] * 1000 <= vmax [urad/s] * duration [ms];
     * a floating-point speed would misjudge moves exactly at the limit). */
    for (int i = 0; i < 5000; i++) {
        const int32_t q0 = (int32_t)(xorshift64(&seed) % 5200001u) - 2600000;
        const int32_t q1 = (int32_t)(xorshift64(&seed) % 4800001u) - 2400000;
        const uint32_t ms = 1u + (uint32_t)(xorshift64(&seed) % 6000u);
        const arb_cmd_t c = make_cmd(1, q0, q1, ms);
        const arb_verdict_t v = decide_once(&t, &c, HOME);
        const uint64_t d0 = (uint64_t)llabs((long long)q0);
        const uint64_t d1 = (uint64_t)llabs((long long)q1);
        const uint64_t budget = (uint64_t)t.vmax_urad_s * (uint64_t)v.duration_ms;

        CHECK(v.verdict == ARB_VERDICT_ALLOW || v.verdict == ARB_VERDICT_CLAMP);
        CHECK(d0 * 1000u <= budget && d1 * 1000u <= budget);
        CHECK(v.duration_ms >= ms);
        if (v.verdict == ARB_VERDICT_CLAMP) {
            /* and the stretch is minimal: 1 ms less would exceed vmax */
            const uint64_t less = (uint64_t)t.vmax_urad_s * (uint64_t)(v.duration_ms - 1u);
            CHECK(d0 * 1000u > less || d1 * 1000u > less);
        }
    }
}

/* ---------------------------------------------------------- FR-G4: keep-out */

static void fr_g4_keepout_target_inside_zone_denied(void)
{
    const arb_contract_table_t t = make_table(1);
    const arb_cmd_t c = make_cmd(1, 785398, 0, 5000);  /* 45 deg: elbow at (212, 212) mm, inside */
    const arb_verdict_t v = decide_once(&t, &c, HOME);

    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
}

static void fr_g4_keepout_path_through_zone_denied_though_endpoints_clear(void)
{
    const arb_contract_table_t t = make_table(1);
    const arb_cmd_t c = make_cmd(1, 1500000, 0, 5000);  /* sweep 0 -> 1.5 rad: elbow crosses the zone */
    double q[2], e[2], p[2];
    arb_verdict_t v;

    /* Both endpoints are outside the zone ... */
    q[0] = 0.0; q[1] = 0.0; arb_fk(q, e, p);
    CHECK(!(e[0] > 200000 && e[0] < 400000 && e[1] > 150000 && e[1] < 350000));
    q[0] = 1.5; arb_fk(q, e, p);
    CHECK(!(e[0] > 200000 && e[0] < 400000 && e[1] > 150000 && e[1] < 350000));
    CHECK(!(p[0] > 200000 && p[0] < 400000 && p[1] > 150000 && p[1] < 350000));
    /* ... but the path is not. */
    v = decide_once(&t, &c, HOME);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
}

static void fr_g4_keepout_clear_path_allowed(void)
{
    const arb_contract_table_t t = make_table(1);
    const arb_cmd_t c = make_cmd(1, -1500000, 500000, 5000);  /* away from the zone */
    const arb_verdict_t v = decide_once(&t, &c, HOME);

    CHECK(v.verdict == ARB_VERDICT_ALLOW);
}

/* Angle (urad, rounded down) that puts the elbow at x = x_um, upper half plane. */
static int32_t elbow_angle_for_x(double x_um)
{
    return (int32_t)floor(acos(x_um / (double)ARB_LINK1_UM) * 1e6);
}

static void fr_g4_keepout_just_outside_by_less_than_margin_denied(void)
{
    const arb_contract_table_t t = make_table(1);
    /* Elbow ends 0.28..0.5 um left of the zone's x_min edge (y inside the band). */
    const int32_t q = elbow_angle_for_x(199999.5);
    const int32_t start[2] = { q + 100, 0 };             /* start further from the zone */
    const arb_cmd_t c = make_cmd(1, q, 0, 5000);
    const arb_verdict_t v = decide_once(&t, &c, start);

    /* Within the conservative margin: fail closed. */
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
}

static void fr_g4_keepout_just_outside_by_more_than_margin_allowed(void)
{
    /* Margin for this move: ~1.2 um sampling + 4 mm start tolerance at 1 rad/s. */
    const arb_contract_table_t t = make_table(1);
    const int32_t q = elbow_angle_for_x(195000.0);       /* 5 mm clear of the edge */
    const int32_t start[2] = { q + 100, 0 };
    const arb_cmd_t c = make_cmd(1, q, 0, 5000);
    const arb_verdict_t v = decide_once(&t, &c, start);

    CHECK(v.verdict == ARB_VERDICT_ALLOW);
}

static void fr_g4_keepout_brief_crossing_of_thin_zone_denied(void)
{
    /* A path that crosses a 1 mm strip in a few samples, with the depth never
     * increasing after the first sample inside. Only the "enters a zone it was
     * not in" rule catches this (found by a random search of paths and zones). */
    arb_contract_table_t t = make_table(1);
    const int32_t start[2] = { 801360, 1515683 };
    const arb_cmd_t c = make_cmd(1, -640195, -1506201, 5000);
    arb_verdict_t v;

    t.zones[0].x_min_um = 267546;
    t.zones[0].y_min_um = 405742;
    t.zones[0].x_max_um = 287546;
    t.zones[0].y_max_um = 406742;
    seal(&t);
    v = decide_once(&t, &c, start);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
}

static void fr_g4_keepout_margin_includes_start_tolerance(void)
{
    /* The arm may start up to 5 ms at vmax (5 mrad at 1 rad/s) from the checked
     * start, so every zone is widened by (L1 + 2 L2) * 5 mrad = 4 mm. An elbow
     * ending 3 mm outside the zone is therefore denied; 5 mm outside is allowed
     * (fr_g4_keepout_just_outside_by_more_than_margin_allowed). */
    const arb_contract_table_t t = make_table(1);
    const int32_t q = elbow_angle_for_x(197000.0);       /* 3 mm clear of the edge */
    const int32_t start[2] = { q + 100, 0 };
    const arb_cmd_t c = make_cmd(1, q, 0, 5000);
    arb_contract_table_t slow = t;
    arb_verdict_t v;

    v = decide_once(&t, &c, start);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
    /* At 0.25 rad/s the tolerance is 1.25 mrad, margin 1 mm: 3 mm clear is allowed. */
    slow.vmax_urad_s = 250000;
    slow.max_duration_ms = 30000;
    seal(&slow);
    v = decide_once(&slow, &c, start);
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
}

static void fr_g4_keepout_overrides_clamp(void)
{
    const arb_contract_table_t t = make_table(1);
    const arb_cmd_t c = make_cmd(1, 785398, 0, 1);       /* too fast and into the zone */
    const arb_verdict_t v = decide_once(&t, &c, HOME);

    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
}

/* --------------------------------------------- FR-G4: leaving a keep-out zone */

static const int32_t ELBOW_IN_ZONE[2] = { 785398, 0 };   /* elbow at (212, 212) mm: 12 mm inside x_min */

static void fr_g4_keepout_exit_only_move_from_inside_allowed(void)
{
    const arb_contract_table_t t = make_table(1);
    const arb_cmd_t c = make_cmd(1, 1200000, 0, 5000);   /* elbow swings out past x_min, tip stays above */
    arb_guard_t g;
    arb_verdict_t v;

    arb_guard_init(&g, &t, T0);
    arb_guard_check(&g, &c, sizeof c, ELBOW_IN_ZONE, T0, &v);
    CHECK(v.verdict == ARB_VERDICT_ALLOW && v.rule == ARB_RULE_NONE);
    CHECK((v.flags & ARB_VFLAG_KEEPOUT_EXIT) != 0u);
    CHECK_EQ_INT(g.violation_fill, 0);                    /* an exit is not a violation */
}

static void fr_g4_keepout_from_inside_going_deeper_denied(void)
{
    const arb_contract_table_t t = make_table(1);
    /* Toward q1 = 0.5 rad: ends outside (below y_min), but first goes deeper (x grows). */
    const arb_cmd_t c = make_cmd(1, 500000, 0, 5000);
    const arb_verdict_t v = decide_once(&t, &c, ELBOW_IN_ZONE);

    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
}

static void fr_g4_keepout_from_inside_not_ending_outside_denied(void)
{
    const arb_contract_table_t t = make_table(1);
    const arb_cmd_t c = make_cmd(1, 800000, 0, 5000);    /* shallower, but still inside */
    const arb_verdict_t v = decide_once(&t, &c, ELBOW_IN_ZONE);

    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
}

static void fr_g4_keepout_exit_through_another_zone_denied(void)
{
    arb_contract_table_t t = make_table(1);
    const arb_cmd_t c = make_cmd(1, 1200000, 0, 5000);   /* the exit move above, ... */
    arb_verdict_t v;

    /* ... crossing a second zone mid-path: the elbow passes (162, 252) mm at
     * q1 = 1.0 rad, then ends at (109, 280) mm, outside both zones. */
    t.zone_count = 2;
    t.zones[1].x_min_um = 150000;
    t.zones[1].y_min_um = 240000;
    t.zones[1].x_max_um = 175000;
    t.zones[1].y_max_um = 265000;
    seal(&t);
    v = decide_once(&t, &c, ELBOW_IN_ZONE);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_KEEPOUT);
}

static void fr_g4_keepout_normal_moves_carry_no_exit_flag(void)
{
    const arb_contract_table_t t = make_table(1);
    const arb_cmd_t c = make_cmd(1, -100000, 0, 1000);
    const arb_verdict_t v = decide_once(&t, &c, HOME);

    CHECK(v.verdict == ARB_VERDICT_ALLOW && v.flags == 0u);
}

/* -------------------------------------------------------- FR-G4: check order */

static void fr_g4_checks_run_in_the_specified_order(void)
{
    const arb_contract_table_t t = make_table(1);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;

    /* Malformed beats everything. */
    c = make_cmd(1, 9000000, 0, 0);                       /* bad flags, bad duration, beyond limits */
    c.flags = 1u;
    v = decide_once(&t, &c, HOME);
    CHECK(v.rule == ARB_RULE_MALFORMED);

    /* Duration bounds (part of well-formed) beat sequence and joint limits. */
    arb_guard_init(&g, &t, T0);
    c = make_cmd(5, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    c = make_cmd(4, 9000000, 0, 0);                       /* old seq, beyond limits, zero duration */
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK(v.rule == ARB_RULE_DURATION);

    /* Sequence beats joint limits. */
    arb_guard_init(&g, &t, T0);
    c = make_cmd(5, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    c = make_cmd(4, 9000000, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK(v.rule == ARB_RULE_SEQUENCE);

    /* Rate beats joint limits. */
    arb_guard_init(&g, &t, T0);
    for (uint32_t i = 1; i <= 10; i++) {
        c = make_cmd(i, 0, 0, 10);
        arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    }
    c = make_cmd(11, 9000000, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK(v.rule == ARB_RULE_RATE);

    /* Joint limits beat velocity and keep-out. */
    c = make_cmd(1, 785398, 2400001, 1);                  /* into the zone, too fast, beyond joint 2 */
    v = decide_once(&t, &c, HOME);
    CHECK(v.rule == ARB_RULE_JOINT_LIMIT);
}

/* ---------------------------------------------------------------- FR-G5 */

static void fr_g5_starts_in_hold_and_valid_command_enters_normal(void)
{
    const arb_contract_table_t t = make_table(0);
    const arb_cmd_t c = make_cmd(1, 0, 0, 10);
    arb_guard_t g;
    arb_verdict_t v;

    arb_guard_init(&g, &t, T0);
    CHECK_EQ_INT(g.state, ARB_STATE_HOLD);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK_EQ_INT(v.state_before, ARB_STATE_HOLD);
    CHECK_EQ_INT(v.state_after, ARB_STATE_NORMAL);
}

static void fr_g5_silence_past_timeout_enters_hold(void)
{
    const arb_contract_table_t t = make_table(0);          /* silence timeout 250 ms */
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c = make_cmd(1, 0, 0, 10);

    arb_guard_init(&g, &t, T0);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK_EQ_INT(arb_guard_tick(&g, T0 + MS(250)), 0);     /* exactly at the timeout: still NORMAL */
    CHECK_EQ_INT(g.state, ARB_STATE_NORMAL);
    CHECK_EQ_INT(arb_guard_tick(&g, T0 + MS(250) + 1u), 1); /* 1 ns later: HOLD */
    CHECK_EQ_INT(g.state, ARB_STATE_HOLD);
    CHECK_EQ_INT(arb_guard_tick(&g, T0 + MS(900)), 0);     /* already in HOLD: no change */
    c = make_cmd(2, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(1000), &v);
    CHECK_EQ_INT(v.state_after, ARB_STATE_NORMAL);         /* next valid command resumes */
}

static void fr_g5_denied_commands_do_not_reset_silence(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c = make_cmd(1, 0, 0, 10);

    arb_guard_init(&g, &t, T0);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    c = make_cmd(1, 0, 0, 10);                              /* replay at 200 ms: denied */
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(200), &v);
    CHECK_EQ_INT(arb_guard_tick(&g, T0 + MS(251)), 1);      /* silence counted from the last accept */
}

static void fr_g5_violation_threshold_enters_safe_stop(void)
{
    const arb_contract_table_t t = make_table(1);          /* N = 3 within W = 1000 ms */
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;

    arb_guard_init(&g, &t, T0);
    c = make_cmd(1, 2600001, 0, 5000);                     /* joint-limit violation */
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK_EQ_INT(v.state_after, ARB_STATE_HOLD);
    c = make_cmd(2, 785398, 0, 5000);                      /* keep-out violation */
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(100), &v);
    CHECK_EQ_INT(v.state_after, ARB_STATE_HOLD);
    c = make_cmd(3, 0, 2400001, 5000);                     /* third within 1 s */
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(999), &v);
    CHECK_EQ_INT(v.state_after, ARB_STATE_SAFE_STOP);
    CHECK_EQ_INT(g.stop_cause, ARB_STOP_VIOLATIONS);
}

static void fr_g5_violations_spread_beyond_window_do_not_stop(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;

    arb_guard_init(&g, &t, T0);
    for (uint32_t i = 0; i < 10; i++) {                    /* one every 600 ms: never 3 within 1000 ms */
        c = make_cmd(i + 1u, 2600001, 0, 5000);
        arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(600u * i), &v);
        CHECK(v.state_after != ARB_STATE_SAFE_STOP);
    }
    c = make_cmd(100, 2600001, 0, 5000);                   /* window edge: 3rd at exactly +1000 ms is outside */
    arb_guard_init(&g, &t, T0);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v); c.seq++;
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(500), &v); c.seq++;
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(1000), &v);
    CHECK(v.state_after != ARB_STATE_SAFE_STOP);
}

static void fr_g5_only_joint_and_keepout_denials_count(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;
    uint8_t junk[7] = { 0 };

    arb_guard_init(&g, &t, T0);
    for (uint32_t i = 0; i < 50; i++) {                    /* malformed */
        arb_guard_check(&g, junk, sizeof junk, HOME, T0, &v);
    }
    c = make_cmd(10, 0, 0, 10);
    for (uint32_t i = 0; i < 50; i++) {                    /* replays and rate denials */
        arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    }
    for (uint32_t i = 0; i < 50; i++) {                    /* duration out of bounds */
        c = make_cmd(1000u + i, 0, 0, 0);
        arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    }
    for (uint32_t i = 0; i < 50; i++) {                    /* rate denials with fresh sequence numbers */
        c = make_cmd(100u + i, 0, 0, 10);
        arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    }
    CHECK(g.state != ARB_STATE_SAFE_STOP);
    CHECK_EQ_INT(g.violation_fill, 0);
}

static void fr_g5_safe_stop_denies_until_operator_reset(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c;

    arb_guard_init(&g, &t, T0);
    CHECK_EQ_INT(arb_guard_reset(&g, T0), -1);             /* reset only from SAFE_STOP */
    arb_guard_fault(&g, ARB_STOP_ESTOP);
    CHECK_EQ_INT(g.state, ARB_STATE_SAFE_STOP);
    c = make_cmd(1, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(1), &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_STATE);
    CHECK_EQ_INT(arb_guard_tick(&g, T0 + MS(5000)), 0);    /* silence does not leave SAFE_STOP */
    CHECK_EQ_INT(g.state, ARB_STATE_SAFE_STOP);
    CHECK_EQ_INT(arb_guard_reset(&g, T0 + MS(6000)), 0);
    CHECK_EQ_INT(g.state, ARB_STATE_HOLD);
    c = make_cmd(2, 0, 0, 10);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(6001), &v);
    CHECK_EQ_INT(v.state_after, ARB_STATE_NORMAL);
}

static void fr_g5_replay_protection_survives_reset(void)
{
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t c = make_cmd(50, 0, 0, 10);

    arb_guard_init(&g, &t, T0);
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    arb_guard_fault(&g, ARB_STOP_WATCHDOG);
    CHECK_EQ_INT(arb_guard_reset(&g, T0 + MS(10)), 0);
    arb_guard_check(&g, &c, sizeof c, HOME, T0 + MS(20), &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_SEQUENCE);
}

static void fr_g9_arm_send_timeout_fault_enters_safe_stop(void)
{
    /* Decision-logic half of FR-G9: the guardian reports an arm-send timeout as
     * a fault, which must enter SAFE_STOP. (The timeout itself is tested on the
     * target with the guardian: fr_g9_arm_never_replies_times_out_and_safe_stops
     * in test_guardian.c.) */
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;

    arb_guard_init(&g, &t, T0);
    arb_guard_fault(&g, ARB_STOP_ARM_TIMEOUT);
    CHECK_EQ_INT(g.state, ARB_STATE_SAFE_STOP);
    CHECK_EQ_INT(g.stop_cause, ARB_STOP_ARM_TIMEOUT);
    arb_guard_fault(&g, ARB_STOP_ESTOP);                   /* first cause is kept */
    CHECK_EQ_INT(g.stop_cause, ARB_STOP_ARM_TIMEOUT);
}

/* ------------------------------------------- three-step decision (guardian) */

static void fr_g4_three_step_decision_matches_one_call(void)
{
    /* The guardian runs pre -> motion -> commit; it must decide exactly as the
     * single call does, over a mix of commands. */
    const arb_contract_table_t t = make_table(1);
    arb_guard_t a, b;
    uint64_t seed = 777u;

    arb_guard_init(&a, &t, T0);
    arb_guard_init(&b, &t, T0);
    for (uint32_t i = 0; i < 2000; i++) {
        const int32_t q0 = (int32_t)(xorshift64(&seed) % 6000001u) - 3000000;
        const int32_t q1 = (int32_t)(xorshift64(&seed) % 6000001u) - 3000000;
        const uint32_t ms = (uint32_t)(xorshift64(&seed) % 12000u);
        const uint32_t seq = (uint32_t)(xorshift64(&seed) % 3000u);
        const uint64_t now = T0 + MS(i * 7u);
        const arb_cmd_t c = make_cmd(seq, q0, q1, ms);
        arb_verdict_t va, vb;
        arb_cmd_t parsed;

        arb_guard_check(&a, &c, sizeof c, HOME, now, &va);
        if (arb_guard_check_pre(&b, &c, sizeof c, now, &parsed, &vb)) {
            arb_guard_check_motion(&b, &parsed, HOME, now, &vb);
            if (vb.verdict != ARB_VERDICT_DENY) {
                arb_guard_commit_accept(&b, now, &vb);
            }
        }
        if (memcmp(&va, &vb, sizeof va) != 0 || a.state != b.state) {
            printf("  command %u decided differently\n", (unsigned)i);
            CHECK(0);
            break;
        }
    }
}

static void fr_g4_pre_check_rejects_without_arm_position(void)
{
    /* Steps 1-5 decide malformed, replayed, rate-limited and out-of-limit
     * commands without the arm's position: the guardian never asks the arm. */
    const arb_contract_table_t t = make_table(0);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t parsed, c;
    uint8_t junk[5] = { 0 };

    arb_guard_init(&g, &t, T0);
    CHECK_EQ_INT(arb_guard_check_pre(&g, junk, sizeof junk, T0, &parsed, &v), 0);
    CHECK(v.rule == ARB_RULE_MALFORMED);
    c = make_cmd(1, 2600001, 0, 100);
    CHECK_EQ_INT(arb_guard_check_pre(&g, &c, sizeof c, T0, &parsed, &v), 0);
    CHECK(v.rule == ARB_RULE_JOINT_LIMIT);
    c = make_cmd(1, 0, 0, 100);                           /* replay of seq 1 */
    CHECK_EQ_INT(arb_guard_check_pre(&g, &c, sizeof c, T0, &parsed, &v), 0);
    CHECK(v.rule == ARB_RULE_SEQUENCE);
    c = make_cmd(2, 0, 0, 100);
    CHECK_EQ_INT(arb_guard_check_pre(&g, &c, sizeof c, T0, &parsed, &v), 1);
    CHECK_EQ_INT(parsed.seq, 2);
}

static void fr_g5_approval_refused_by_arm_is_not_activity(void)
{
    const arb_contract_table_t t = make_table(0);
    const arb_cmd_t c = make_cmd(1, 100000, 0, 1000);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t parsed;

    arb_guard_init(&g, &t, T0);
    CHECK_EQ_INT(arb_guard_check_pre(&g, &c, sizeof c, T0 + MS(10), &parsed, &v), 1);
    arb_guard_check_motion(&g, &parsed, HOME, T0 + MS(10), &v);
    CHECK(v.verdict == ARB_VERDICT_ALLOW);
    arb_guard_reject_after_approval(&g, ARB_RULE_STALE_START, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_STALE_START);
    CHECK_EQ_INT(v.target_urad[0], 0);                     /* a DENY carries no setpoint */
    CHECK_EQ_INT(g.state, ARB_STATE_HOLD);                 /* still HOLD: nothing was accepted */
    CHECK_EQ_INT(g.last_accept_ns, T0);
    CHECK_EQ_INT(g.violation_fill, 0);                     /* and not a violation */
}

static void fr_g9_arm_fault_denial_carries_no_setpoint(void)
{
    const arb_contract_table_t t = make_table(0);
    const arb_cmd_t c = make_cmd(1, 100000, 0, 1000);
    arb_guard_t g;
    arb_verdict_t v;
    arb_cmd_t parsed;

    arb_guard_init(&g, &t, T0);
    CHECK_EQ_INT(arb_guard_check_pre(&g, &c, sizeof c, T0, &parsed, &v), 1);
    arb_guard_check_motion(&g, &parsed, HOME, T0, &v);
    arb_guard_fault(&g, ARB_STOP_ARM_TIMEOUT);
    arb_guard_reject_after_approval(&g, ARB_RULE_ARM_FAULT, &v);
    CHECK(v.verdict == ARB_VERDICT_DENY && v.rule == ARB_RULE_ARM_FAULT);
    CHECK_EQ_INT(v.duration_ms, 0);
    CHECK_EQ_INT(v.state_after, ARB_STATE_SAFE_STOP);
}

static void fr_g5_last_client_lost_enters_hold(void)
{
    const arb_contract_table_t t = make_table(0);
    const arb_cmd_t c = make_cmd(1, 0, 0, 10);
    arb_guard_t g;
    arb_verdict_t v;

    arb_guard_init(&g, &t, T0);
    CHECK_EQ_INT(arb_guard_client_lost(&g), 0);           /* already HOLD */
    arb_guard_check(&g, &c, sizeof c, HOME, T0, &v);
    CHECK_EQ_INT(g.state, ARB_STATE_NORMAL);
    CHECK_EQ_INT(arb_guard_client_lost(&g), 1);           /* at once, no timeout */
    CHECK_EQ_INT(g.state, ARB_STATE_HOLD);
    arb_guard_fault(&g, ARB_STOP_ESTOP);
    CHECK_EQ_INT(arb_guard_client_lost(&g), 0);           /* never leaves SAFE_STOP */
    CHECK_EQ_INT(g.state, ARB_STATE_SAFE_STOP);
}

/* ------------------------------------------------------------- kinematics */

static void fr_g4_forward_kinematics_reference_poses(void)
{
    double q[2], e[2], p[2];

    q[0] = 0.0; q[1] = 0.0; arb_fk(q, e, p);
    CHECK(fabs(e[0] - 300000.0) < 1e-6 && fabs(e[1]) < 1e-6);
    CHECK(fabs(p[0] - 550000.0) < 1e-6 && fabs(p[1]) < 1e-6);
    q[0] = acos(-1.0) / 2.0; q[1] = -acos(-1.0) / 2.0; arb_fk(q, e, p);  /* elbow up, forearm along +x */
    CHECK(fabs(e[0]) < 1e-6 && fabs(e[1] - 300000.0) < 1e-6);
    CHECK(fabs(p[0] - 250000.0) < 1e-6 && fabs(p[1] - 300000.0) < 1e-6);
}

/* ---------------------------------------------------------------------- run */

static const arb_test_case_t cases[] = {
    ARB_TEST_CASE(fr_c4_crc32_matches_zlib_check_value),
    ARB_TEST_CASE(fr_c4_valid_table_loads),
    ARB_TEST_CASE(fr_c4_null_arguments_rejected),
    ARB_TEST_CASE(fr_c4_wrong_length_rejected),
    ARB_TEST_CASE(fr_c4_bad_magic_rejected),
    ARB_TEST_CASE(fr_c4_version_mismatch_rejected),
    ARB_TEST_CASE(fr_c4_table_size_field_mismatch_rejected),
    ARB_TEST_CASE(fr_c4_crc_mismatch_rejected),
    ARB_TEST_CASE(fr_c4_any_single_byte_change_rejected),
    ARB_TEST_CASE(fr_c4_out_of_range_values_rejected),
    ARB_TEST_CASE(fr_c4_max_duration_must_cover_a_full_range_move),
    ARB_TEST_CASE(fr_c4_output_untouched_on_error),
    ARB_TEST_CASE(fr_c5_text_contract_rejected),
    ARB_TEST_CASE(nfr_i6_command_wrong_length_rejected),
    ARB_TEST_CASE(nfr_i6_command_bad_fields_rejected),
    ARB_TEST_CASE(nfr_i6_random_bytes_never_crash_or_allow_garbage),
    ARB_TEST_CASE(fr_g2_command_fields_carried_through),
    ARB_TEST_CASE(fr_g2_duration_zero_denied),
    ARB_TEST_CASE(fr_g2_duration_minimum_and_contract_max_allowed),
    ARB_TEST_CASE(fr_g2_duration_one_over_contract_max_denied),
    ARB_TEST_CASE(fr_g2_clamp_beyond_contract_max_denied),
    ARB_TEST_CASE(fr_g3_every_verdict_names_its_rule),
    ARB_TEST_CASE(fr_g4_sequence_replay_and_old_denied),
    ARB_TEST_CASE(fr_g4_sequence_wraps_around),
    ARB_TEST_CASE(fr_g4_rate_limit_burst_then_deny_then_refill),
    ARB_TEST_CASE(fr_g4_rate_limit_uses_guardian_clock_not_sender_clock),
    ARB_TEST_CASE(fr_g4_rate_limit_sustained_rate_holds),
    ARB_TEST_CASE(fr_g4_joint_limit_exactly_at_max),
    ARB_TEST_CASE(fr_g4_joint_limit_just_inside),
    ARB_TEST_CASE(fr_g4_joint_limit_just_outside_denied),
    ARB_TEST_CASE(fr_g4_impossible_arm_position_fails_closed),
    ARB_TEST_CASE(fr_g4_unusable_contract_fails_closed),
    ARB_TEST_CASE(fr_g4_velocity_exactly_at_limit_allowed),
    ARB_TEST_CASE(fr_g4_velocity_over_limit_clamped_by_stretching_duration),
    ARB_TEST_CASE(fr_g4_velocity_clamped_motion_never_exceeds_vmax),
    ARB_TEST_CASE(fr_g4_keepout_target_inside_zone_denied),
    ARB_TEST_CASE(fr_g4_keepout_path_through_zone_denied_though_endpoints_clear),
    ARB_TEST_CASE(fr_g4_keepout_clear_path_allowed),
    ARB_TEST_CASE(fr_g4_keepout_just_outside_by_less_than_margin_denied),
    ARB_TEST_CASE(fr_g4_keepout_just_outside_by_more_than_margin_allowed),
    ARB_TEST_CASE(fr_g4_keepout_brief_crossing_of_thin_zone_denied),
    ARB_TEST_CASE(fr_g4_keepout_margin_includes_start_tolerance),
    ARB_TEST_CASE(fr_g4_keepout_overrides_clamp),
    ARB_TEST_CASE(fr_g4_keepout_exit_only_move_from_inside_allowed),
    ARB_TEST_CASE(fr_g4_keepout_from_inside_going_deeper_denied),
    ARB_TEST_CASE(fr_g4_keepout_from_inside_not_ending_outside_denied),
    ARB_TEST_CASE(fr_g4_keepout_exit_through_another_zone_denied),
    ARB_TEST_CASE(fr_g4_keepout_normal_moves_carry_no_exit_flag),
    ARB_TEST_CASE(fr_g4_checks_run_in_the_specified_order),
    ARB_TEST_CASE(fr_g4_forward_kinematics_reference_poses),
    ARB_TEST_CASE(fr_g5_starts_in_hold_and_valid_command_enters_normal),
    ARB_TEST_CASE(fr_g5_silence_past_timeout_enters_hold),
    ARB_TEST_CASE(fr_g5_denied_commands_do_not_reset_silence),
    ARB_TEST_CASE(fr_g5_violation_threshold_enters_safe_stop),
    ARB_TEST_CASE(fr_g5_violations_spread_beyond_window_do_not_stop),
    ARB_TEST_CASE(fr_g5_only_joint_and_keepout_denials_count),
    ARB_TEST_CASE(fr_g5_safe_stop_denies_until_operator_reset),
    ARB_TEST_CASE(fr_g5_replay_protection_survives_reset),
    ARB_TEST_CASE(fr_g9_arm_send_timeout_fault_enters_safe_stop),
    ARB_TEST_CASE(fr_g4_three_step_decision_matches_one_call),
    ARB_TEST_CASE(fr_g4_pre_check_rejects_without_arm_position),
    ARB_TEST_CASE(fr_g5_approval_refused_by_arm_is_not_activity),
    ARB_TEST_CASE(fr_g9_arm_fault_denial_carries_no_setpoint),
    ARB_TEST_CASE(fr_g5_last_client_lost_enters_hold),
};

int main(void)
{
    return arb_test_run("test_libcontract", cases, (int)(sizeof cases / sizeof cases[0]));
}
