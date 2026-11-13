/*
 * Unit tests for the arm's motion model (qnx/arm/arm_motion.c), on the host
 * (ASan, UBSan) and on QNX. Names start with the requirement they cover.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arb_arm_model.h"
#include "arm_backend.h"
#include "arm_motion.h"
#include "arb_test.h"

#define MS(x) ((int64_t)(x) * 1000000)
#define T0    MS(5000)
#define VMAX  1000000u        /* 1 rad/s, as in contracts/default.yaml */

static uint64_t xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static int64_t iabs(int64_t v) { return v < 0 ? -v : v; }

/* Distance (urad) of p from the line start->target in joint space, scaled:
 * |cross(p - start, target - start)| / |target - start|. */
static double off_line(const int32_t s[2], const int32_t t[2], const int32_t p[2])
{
    const double dx = (double)t[0] - s[0], dy = (double)t[1] - s[1];
    const double px = (double)p[0] - s[0], py = (double)p[1] - s[1];
    const double len = dx * dx + dy * dy;
    double c;

    if (len == 0.0) {
        return 0.0;
    }
    c = px * dy - py * dx;
    return (c < 0 ? -c : c) / __builtin_sqrt(len);
}

static void fr_a3_motion_reaches_target_at_its_duration(void)
{
    const int32_t from[2] = { 0, 0 }, to[2] = { 500000, -250000 };
    arm_motion_t m;
    arm_sample_t s;

    arm_motion_init(&m, from, T0);
    arm_motion_start(&m, from, to, 500, T0);
    arm_motion_sample(&m, T0 + MS(250), &s);
    CHECK_EQ_INT(s.state, ARM_STATE_MOVING);
    CHECK_EQ_INT(s.pos_urad[0], 250000);
    CHECK_EQ_INT(s.pos_urad[1], -125000);
    CHECK_EQ_INT(s.vel_urad_s[0], 1000000);
    arm_motion_sample(&m, T0 + MS(500), &s);
    CHECK_EQ_INT(s.state, ARM_STATE_IDLE);
    CHECK_EQ_INT(s.pos_urad[0], 500000);
    CHECK_EQ_INT(s.pos_urad[1], -250000);
    CHECK_EQ_INT(s.vel_urad_s[0], 0);
    arm_motion_sample(&m, T0 + MS(9000), &s);           /* and stays there */
    CHECK_EQ_INT(s.pos_urad[0], 500000);
}

static void fr_a3_motion_is_a_straight_line_with_joints_arriving_together(void)
{
    uint64_t seed = 99u;

    for (int k = 0; k < 200; k++) {
        const int32_t from[2] = { (int32_t)(xorshift64(&seed) % 5000001u) - 2500000,
                                  (int32_t)(xorshift64(&seed) % 4000001u) - 2000000 };
        const int32_t to[2] = { (int32_t)(xorshift64(&seed) % 5000001u) - 2500000,
                                (int32_t)(xorshift64(&seed) % 4000001u) - 2000000 };
        const uint32_t ms = arm_motion_min_duration_ms(from, to, VMAX) + (uint32_t)(xorshift64(&seed) % 500u);
        arm_motion_t m;
        arm_sample_t s;

        arm_motion_init(&m, from, T0);
        arm_motion_start(&m, from, to, ms, T0);
        for (uint32_t t = 0; t <= ms; t += 7u) {
            arm_motion_sample(&m, T0 + MS(t), &s);
            if (off_line(from, to, s.pos_urad) > 2.0) {  /* integer rounding only */
                CHECK(0);
                return;
            }
        }
        arm_motion_sample(&m, T0 + MS(ms), &s);
        CHECK(s.pos_urad[0] == to[0] && s.pos_urad[1] == to[1]);
    }
}

static void fr_a3_never_faster_than_vmax_at_the_minimum_duration(void)
{
    uint64_t seed = 1234u;

    for (int k = 0; k < 200; k++) {
        const int32_t from[2] = { (int32_t)(xorshift64(&seed) % 5000001u) - 2500000,
                                  (int32_t)(xorshift64(&seed) % 4000001u) - 2000000 };
        const int32_t to[2] = { (int32_t)(xorshift64(&seed) % 5000001u) - 2500000,
                                (int32_t)(xorshift64(&seed) % 4000001u) - 2000000 };
        const uint32_t ms = arm_motion_min_duration_ms(from, to, VMAX);
        arm_motion_t m;
        arm_sample_t a, b;

        arm_motion_init(&m, from, T0);
        arm_motion_start(&m, from, to, ms, T0);
        arm_motion_sample(&m, T0, &a);
        for (uint32_t t = 1; t <= ms + 2u; t++) {          /* every 1 ms, like the control loop */
            arm_motion_sample(&m, T0 + MS(t), &b);
            for (int j = 0; j < 2; j++) {
                /* displacement in 1 ms <= vmax * 1 ms, +1 urad for rounding */
                if (iabs((int64_t)b.pos_urad[j] - a.pos_urad[j]) > (int64_t)(VMAX / 1000u) + 1 ||
                    iabs(b.vel_urad_s[j]) > (int64_t)VMAX) {
                    CHECK(0);
                    return;
                }
            }
            a = b;
        }
    }
}

static void fr_a3_min_duration_matches_the_guardian_rule(void)
{
    const int32_t z[2] = { 0, 0 };
    int32_t to[2] = { 500000, 0 };

    CHECK_EQ_INT(arm_motion_min_duration_ms(z, to, VMAX), 500);
    to[0] = 500001;
    CHECK_EQ_INT(arm_motion_min_duration_ms(z, to, VMAX), 501);   /* ceil, like libcontract */
    to[0] = 0; to[1] = -300000;
    CHECK_EQ_INT(arm_motion_min_duration_ms(z, to, VMAX), 300);   /* the faster joint decides */
    CHECK_EQ_INT(arm_motion_min_duration_ms(z, z, VMAX), 1);
    CHECK_EQ_INT(arm_motion_min_duration_ms(z, to, 0), UINT32_MAX); /* no speed: no move */
}

static void fr_a3_brake_stays_on_segment(void)
{
    uint64_t seed = 4242u;

    for (int k = 0; k < 300; k++) {
        const int32_t from[2] = { (int32_t)(xorshift64(&seed) % 5000001u) - 2500000,
                                  (int32_t)(xorshift64(&seed) % 4000001u) - 2000000 };
        const int32_t to[2] = { (int32_t)(xorshift64(&seed) % 5000001u) - 2500000,
                                (int32_t)(xorshift64(&seed) % 4000001u) - 2000000 };
        const uint32_t ms = arm_motion_min_duration_ms(from, to, VMAX);
        const int64_t tb = T0 + MS(xorshift64(&seed) % (ms + 1u));
        int64_t prev_progress = -1;
        int64_t prev_speed = INT64_MAX / 2;   /* "no previous sample"; leaves room for +2 */
        arm_motion_t m;
        arm_sample_t s;

        arm_motion_init(&m, from, T0);
        arm_motion_start(&m, from, to, ms, T0);
        arm_motion_stop(&m, tb, 1);
        for (int64_t t = tb; t <= tb + MS(200); t += MS(1)) {
            int64_t progress, speed;
            arm_motion_sample(&m, t, &s);
            /* on the line from start to target ... */
            if (off_line(from, to, s.pos_urad) > 2.0) { CHECK(0); return; }
            /* ... moving only forward, never past the target ... */
            progress = iabs((int64_t)s.pos_urad[0] - from[0]) + iabs((int64_t)s.pos_urad[1] - from[1]);
            if (progress + 2 < prev_progress ||
                progress > iabs((int64_t)to[0] - from[0]) + iabs((int64_t)to[1] - from[1]) + 2) {
                CHECK(0); return;
            }
            /* ... and never speeding up. */
            speed = iabs(s.vel_urad_s[0]) + iabs(s.vel_urad_s[1]);
            if (speed > prev_speed + 2) { CHECK(0); return; }
            prev_progress = progress;
            prev_speed = speed;
        }
        CHECK_EQ_INT(s.state, ARM_STATE_BRAKED);
        CHECK(s.vel_urad_s[0] == 0 && s.vel_urad_s[1] == 0);
    }
}

static void fr_a4_brake_from_full_speed_stops_within_its_braking_time(void)
{
    /* At 1 rad/s and 10 rad/s^2 the stop takes 100 ms and 0.05 rad. */
    const int32_t from[2] = { 0, 0 }, to[2] = { 2000000, 0 };
    arm_motion_t m;
    arm_sample_t s;

    arm_motion_init(&m, from, T0);
    arm_motion_start(&m, from, to, 2000, T0);
    arm_motion_stop(&m, T0 + MS(500), 1);
    arm_motion_sample(&m, T0 + MS(550), &s);
    CHECK_EQ_INT(s.state, ARM_STATE_STOPPING);
    CHECK(s.vel_urad_s[0] > 400000 && s.vel_urad_s[0] < 600000);   /* half speed at half time */
    arm_motion_sample(&m, T0 + MS(601), &s);
    CHECK_EQ_INT(s.state, ARM_STATE_BRAKED);
    CHECK_EQ_INT(s.vel_urad_s[0], 0);
    CHECK(iabs((int64_t)s.pos_urad[0] - 550000) <= 2);             /* 0.5 rad + 0.05 rad */
}

static void fr_a3_hold_stops_without_latching_and_brake_upgrades_it(void)
{
    const int32_t from[2] = { 0, 0 }, to[2] = { 1000000, 0 };
    arm_motion_t m;
    arm_sample_t s;

    arm_motion_init(&m, from, T0);
    arm_motion_start(&m, from, to, 1000, T0);
    arm_motion_stop(&m, T0 + MS(100), 0);                 /* hold */
    arm_motion_sample(&m, T0 + MS(300), &s);
    CHECK_EQ_INT(s.state, ARM_STATE_IDLE);                /* stopped, not latched */
    arm_motion_start(&m, from, to, 1000, T0 + MS(300));
    arm_motion_stop(&m, T0 + MS(400), 0);
    arm_motion_stop(&m, T0 + MS(420), 1);                 /* hold becomes a latched brake */
    arm_motion_sample(&m, T0 + MS(700), &s);
    CHECK_EQ_INT(s.state, ARM_STATE_BRAKED);
}

static void fr_a3_new_segment_starts_from_the_given_actual_position(void)
{
    const int32_t from[2] = { 0, 0 }, to[2] = { 1000000, 0 }, to2[2] = { 0, 500000 };
    arm_motion_t m;
    arm_sample_t s;

    arm_motion_init(&m, from, T0);
    arm_motion_start(&m, from, to, 1000, T0);
    arm_motion_sample(&m, T0 + MS(400), &s);              /* mid-move */
    arm_motion_start(&m, s.pos_urad, to2, 1000, T0 + MS(400));
    arm_motion_sample(&m, T0 + MS(400), &s);
    CHECK_EQ_INT(s.pos_urad[0], 400000);                  /* no jump at the switch */
    arm_motion_sample(&m, T0 + MS(1400), &s);
    CHECK(s.pos_urad[0] == 0 && s.pos_urad[1] == 500000);
}

static void fr_a3_motion_depends_on_time_not_on_tick_count(void)
{
    /* Sampling more often (extra or spoofed ticks) must not move the arm further. */
    const int32_t from[2] = { 0, 0 }, to[2] = { 1000000, 0 };
    arm_motion_t m;
    arm_sample_t once, many;

    arm_motion_init(&m, from, T0);
    arm_motion_start(&m, from, to, 1000, T0);
    arm_motion_sample(&m, T0 + MS(300), &once);
    for (int i = 0; i < 100000; i++) {
        arm_motion_sample(&m, T0 + MS(300), &many);
    }
    CHECK_EQ_INT(once.pos_urad[0], many.pos_urad[0]);
    CHECK_EQ_INT(once.pos_urad[0], 300000);
}

static void fr_a2_model_reports_position_and_velocity_per_joint(void)
{
    const int32_t from[2] = { 100000, -200000 }, to[2] = { 400000, 400000 };
    arm_motion_t m;
    arm_sample_t s;

    arm_motion_init(&m, from, T0);
    arm_motion_start(&m, from, to, 600, T0);              /* joint 2 moves 0.6 rad: 1 rad/s */
    arm_motion_sample(&m, T0 + MS(300), &s);
    CHECK_EQ_INT(s.pos_urad[0], 250000);                  /* each joint has its own position ... */
    CHECK_EQ_INT(s.pos_urad[1], 100000);
    CHECK_EQ_INT(s.vel_urad_s[0], 500000);                /* ... and its own velocity */
    CHECK_EQ_INT(s.vel_urad_s[1], 1000000);
}

static void fr_a5_simulation_sits_behind_the_backend_interface(void)
{
    /* The control loop only calls init/apply/read; a servo backend implements the
     * same three operations. */
    const int32_t home[2] = { 0, 0 }, p[2] = { 1234, -5678 }, v[2] = { 10, 20 };
    int32_t got[2] = { 99, 99 };
    arm_backend_t b;

    arm_backend_sim_bind(&b);
    CHECK(b.name != NULL && b.init != NULL && b.apply != NULL && b.read != NULL);
    CHECK_EQ_INT(b.init(&b, home), 0);
    b.apply(&b, p, v);
    b.read(&b, got);
    CHECK(got[0] == 1234 && got[1] == -5678);
}

static const arb_test_case_t cases[] = {
    ARB_TEST_CASE(fr_a2_model_reports_position_and_velocity_per_joint),
    ARB_TEST_CASE(fr_a5_simulation_sits_behind_the_backend_interface),
    ARB_TEST_CASE(fr_a3_motion_reaches_target_at_its_duration),
    ARB_TEST_CASE(fr_a3_motion_is_a_straight_line_with_joints_arriving_together),
    ARB_TEST_CASE(fr_a3_never_faster_than_vmax_at_the_minimum_duration),
    ARB_TEST_CASE(fr_a3_min_duration_matches_the_guardian_rule),
    ARB_TEST_CASE(fr_a3_brake_stays_on_segment),
    ARB_TEST_CASE(fr_a4_brake_from_full_speed_stops_within_its_braking_time),
    ARB_TEST_CASE(fr_a3_hold_stops_without_latching_and_brake_upgrades_it),
    ARB_TEST_CASE(fr_a3_new_segment_starts_from_the_given_actual_position),
    ARB_TEST_CASE(fr_a3_motion_depends_on_time_not_on_tick_count),
};

int main(void)
{
    return arb_test_run("test_arm_motion", cases, (int)(sizeof cases / sizeof cases[0]));
}
