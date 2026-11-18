/*
 * On-target tests of arb-arm, run by qnx/tests/arm_suite.sh against a freshly
 * started arm. This program plays the guardian (it connects first, so the arm
 * binds to it); a forked child plays an untrusted process. Every check reads the
 * arm's own telemetry ring. QEMU runs prove function; times are reported, not
 * claimed (Pi 5 numbers come in M3).
 *
 * Contract: contracts/default.yaml (vmax 1 rad/s, shoulder +-150 deg, elbow +-135 deg).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <signal.h>
#include <sys/wait.h>

#include "arb_arm_model.h"
#include "arb_arm_proto.h"
#include "arb_prio.h"
#include "arb_pulses.h"
#include "arb_telemetry.h"
#include "arb_watchdog_proto.h"
#include "arm_motion.h"
#include "arb_test.h"

#define VMAX 1000000                       /* urad/s */
#define TEST_PRIO 60                       /* below the trusted range, above default work */

static const arb_telem_t *g_tm;
static int g_coid = -1;
static uint32_t g_seq;
static pid_t g_watchdog;                   /* arm_suite.sh passes the watchdog's pid */

/* ---------------------------------------------------------------- helpers */

static void ms_sleep(long ms)
{
    const struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static double cyc_to_ms(uint64_t c)
{
    return (double)c * 1000.0 / (double)g_tm->cycles_per_sec;
}

static uint64_t tm_head(void)
{
    return atomic_load_explicit(&((arb_telem_t *)g_tm)->head, memory_order_acquire);
}

/* Copy sample i; 0 if it has been overwritten (or is not written yet). */
static int tm_get(uint64_t i, arb_telem_sample_t *out)
{
    return arb_telem_get(g_tm, i, out);
}

static int tm_last(arb_telem_sample_t *out)
{
    const uint64_t h = tm_head();
    return h > 0 && tm_get(h - 1u, out);
}

static uint64_t tm_counter(const _Atomic uint64_t *c)
{
    return atomic_load_explicit((_Atomic uint64_t *)c, memory_order_relaxed);
}

static int connect_arm(void)
{
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html */
    g_coid = name_open(ARB_ARM_NAME, 0);
    return g_coid;
}

static long send_state(int coid, arb_arm_reply_t *r)
{
    arb_arm_get_state_t q;

    memset(&q, 0, sizeof q);
    q.hdr.type = (uint16_t)ARB_ARM_MSG_GET_STATE;
    q.hdr.version = (uint16_t)ARB_ARM_PROTO_VERSION;
    q.hdr.seq = ++g_seq;
    return MsgSend(coid, &q, sizeof q, r, sizeof *r);
}

static long send_setpoint(int coid, const int32_t start[2], const int32_t target[2], uint32_t ms, arb_arm_reply_t *r)
{
    arb_arm_setpoint_t q;

    memset(&q, 0, sizeof q);
    q.hdr.type = (uint16_t)ARB_ARM_MSG_SETPOINT;
    q.hdr.version = (uint16_t)ARB_ARM_PROTO_VERSION;
    q.hdr.seq = ++g_seq;
    memcpy(q.start_urad, start, sizeof q.start_urad);
    memcpy(q.target_urad, target, sizeof q.target_urad);
    q.duration_ms = ms;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html */
    return MsgSend(coid, &q, sizeof q, r, sizeof *r);
}

/* FR-G5: the reset message (the guardian sends it on an operator reset). */
static long send_reset(int coid, arb_arm_reply_t *r)
{
    arb_arm_reset_t q;

    memset(&q, 0, sizeof q);
    q.hdr.type = (uint16_t)ARB_ARM_MSG_RESET;
    q.hdr.version = (uint16_t)ARB_ARM_PROTO_VERSION;
    q.hdr.seq = ++g_seq;
    return MsgSend(coid, &q, sizeof q, r, sizeof *r);
}

static int current(int32_t pos[2])
{
    arb_arm_reply_t r;

    if (send_state(g_coid, &r) != EOK) {
        return -1;
    }
    pos[0] = r.pos_urad[0];
    pos[1] = r.pos_urad[1];
    return 0;
}

/* Setpoint from where the arm is now. Returns the arm's reply status, or -1. */
static int move_to(int32_t q0, int32_t q1, uint32_t ms)
{
    int32_t pos[2] = { 0, 0 }, target[2] = { q0, q1 };
    arb_arm_reply_t r;

    if (current(pos) != 0 || send_setpoint(g_coid, pos, target, ms, &r) != EOK) {
        return -1;
    }
    return r.status;
}

/* Renew the lease every ~1 ms for `ms` milliseconds, as the guardian does. */
static void renew_for(int coid, long ms)
{
    for (long i = 0; i < ms; i++) {
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html */
        (void)MsgSendPulse(coid, ARB_PRIO_ARM, ARB_PULSE_ARM_RENEW, 0);
        ms_sleep(1);
    }
}

/* Wait up to timeout_ms for the latest sample to be in `state`. */
static int wait_state(uint8_t state, long timeout_ms, int renew)
{
    arb_telem_sample_t s;

    for (long i = 0; i < timeout_ms; i++) {
        if (tm_last(&s) && s.state == state) {
            return 1;
        }
        if (renew) {
            (void)MsgSendPulse(g_coid, ARB_PRIO_ARM, ARB_PULSE_ARM_RENEW, 0);
        }
        ms_sleep(1);
    }
    return 0;
}

/* Index of the first sample at or after `from` whose state differs from `state`. */
static uint64_t first_change(uint64_t from, uint8_t state, arb_telem_sample_t *out)
{
    const uint64_t h = tm_head();

    for (uint64_t i = from; i < h; i++) {
        if (tm_get(i, out) && out->state != state) {
            return i;
        }
    }
    return UINT64_MAX;
}

/* ------------------------------------------------------------------ tests */

static void fr_g6_arm_binds_to_the_first_guardian_connection(void)
{
    arb_arm_reply_t r;

    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_tm)->bound_pid), -1);   /* nobody yet */
    CHECK(connect_arm() >= 0);
    CHECK_EQ_INT(send_state(g_coid, &r), EOK);
    CHECK_EQ_INT(r.version, ARB_ARM_PROTO_VERSION);
    CHECK_EQ_INT(r.status, ARB_ARM_OK);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_tm)->bound_pid), getpid());
}

static void fr_a6_telemetry_published_every_cycle(void)
{
    const uint64_t h0 = tm_head();
    uint64_t h1;

    CHECK_EQ_INT(g_tm->magic, ARB_TELEM_MAGIC);
    ms_sleep(1000);
    h1 = tm_head();
    printf("info telemetry: %llu samples in 1 s\n", (unsigned long long)(h1 - h0));
    CHECK(h1 - h0 >= 900u && h1 - h0 <= 1100u);
}

static int cmp_d(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void fr_a1_control_loop_period_measured(void)
{
    static double p[4000];
    const uint64_t h = tm_head();
    arb_telem_sample_t a, b;
    int n = 0;

    const uint64_t first = h > 4001u ? h - 4001u : 0u;

    for (uint64_t i = first; i + 1u < h && n < 4000; i++) {
        if (tm_get(i, &a) && tm_get(i + 1u, &b)) {
            p[n++] = cyc_to_ms(b.t_cycles - a.t_cycles);
        }
    }
    CHECK(n > 1500);
    if (n == 0) {
        return;
    }
    qsort(p, (size_t)n, sizeof p[0], cmp_d);
    printf("info control loop period (QEMU, function only): n=%d min %.3f ms median %.3f ms "
           "p99.9 %.3f ms max %.3f ms\n", n, p[0], p[n / 2], p[(n * 999) / 1000], p[n - 1]);
    CHECK(p[n / 2] > 0.9 && p[n / 2] < 1.1);
}

static void fr_g6_arm_rejects_setpoints_from_another_process(void)
{
    const uint64_t rejected0 = tm_counter(&g_tm->rejected);
    arb_telem_sample_t before, after;
    pid_t child;
    int status = 0;

    CHECK(tm_last(&before));
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fork.html */
    child = fork();
    if (child == 0) {
        const int32_t z[2] = { 0, 0 }, t[2] = { 1000000, 0 };
        arb_arm_reply_t r;
        const int coid = name_open(ARB_ARM_NAME, 0);
        long rc;
        if (coid == -1) {
            _exit(10);
        }
        rc = send_setpoint(coid, z, t, 2000, &r);
        _exit(rc == -1 && errno == EPERM ? 0 : 11);   /* refused with EPERM */
    }
    CHECK(child > 0);
    (void)waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ms_sleep(5);
    CHECK(tm_last(&after));
    CHECK_EQ_INT(after.segment, before.segment);       /* no new motion */
    CHECK(tm_counter(&g_tm->rejected) > rejected0);
}

static void fr_a3_arm_enforces_vmax_itself_and_moves_in_a_straight_line(void)
{
    /* Ask for 0.7 rad in 1 ms (700x too fast). The arm must stretch it to 700 ms. */
    int32_t start[2] = { 0, 0 };
    const int32_t target[2] = { 700000, -350000 };
    arb_telem_sample_t a, b;
    uint64_t i0, h;
    double max_speed = 0.0, max_off = 0.0;
    int reached = 0;

    CHECK_EQ_INT(current(start), 0);
    i0 = tm_head();
    CHECK_EQ_INT(move_to(target[0], target[1], 1), ARB_ARM_OK);
    renew_for(g_coid, 900);
    h = tm_head();
    for (uint64_t i = i0; i + 1u < h; i++) {
        if (!tm_get(i, &a) || !tm_get(i + 1u, &b)) {
            continue;
        }
        for (int j = 0; j < 2; j++) {
            const double v = (double)labs((long)b.vel_urad_s[j]);
            if (v > max_speed) {
                max_speed = v;
            }
        }
        {   /* distance from the start->target line, in urad */
            const double dx = (double)target[0] - start[0], dy = (double)target[1] - start[1];
            const double px = (double)b.pos_urad[0] - start[0], py = (double)b.pos_urad[1] - start[1];
            const double off = (px * dy - py * dx) / __builtin_sqrt(dx * dx + dy * dy);
            if ((off < 0 ? -off : off) > max_off) {
                max_off = off < 0 ? -off : off;
            }
        }
        if (b.pos_urad[0] == target[0] && b.pos_urad[1] == target[1]) {
            reached = 1;
        }
    }
    printf("info fastest joint %.0f urad/s (vmax %d), max distance from the line %.1f urad\n",
           max_speed, VMAX, max_off);
    CHECK(max_speed <= (double)VMAX);
    CHECK(max_off <= 2.0);
    CHECK(reached);
}

static void fr_a3_arm_rejects_targets_beyond_joint_limits(void)
{
    CHECK_EQ_INT(move_to(2700000, 0, 5000), ARB_ARM_REJECTED);   /* beyond 150 deg */
    CHECK_EQ_INT(move_to(0, -2400000, 5000), ARB_ARM_REJECTED);  /* beyond -135 deg */
}

static void fr_a3_stale_start_rejected(void)
{
    int32_t pos[2] = { 0, 0 };
    const int32_t target[2] = { 0, 0 };
    arb_arm_reply_t r;

    CHECK_EQ_INT(current(pos), 0);
    pos[0] += 6000;                                    /* tolerance is 5 mrad at 1 rad/s */
    CHECK_EQ_INT(send_setpoint(g_coid, pos, target, 2000, &r), EOK);
    CHECK_EQ_INT(r.status, ARB_ARM_STALE_START);
}

static void fr_a3_new_setpoint_mid_move_starts_from_actual_position(void)
{
    arb_telem_sample_t a, b;
    uint64_t i0, h;
    int64_t max_step = 0, max_excess = INT64_MIN, at_dt = 0;

    CHECK_EQ_INT(move_to(0, 0, 1000), ARB_ARM_OK);
    renew_for(g_coid, 1100);
    i0 = tm_head();
    CHECK_EQ_INT(move_to(1000000, 0, 1000), ARB_ARM_OK);
    renew_for(g_coid, 400);
    CHECK_EQ_INT(move_to(1000000, 800000, 1000), ARB_ARM_OK);   /* switch mid-move */
    renew_for(g_coid, 1200);
    h = tm_head();
    for (uint64_t i = i0; i + 1u < h; i++) {
        if (tm_get(i, &a) && tm_get(i + 1u, &b)) {
            const int64_t dt = b.t_ns - a.t_ns;
            for (int j = 0; j < 2; j++) {
                const int64_t d = llabs((long long)b.pos_urad[j] - a.pos_urad[j]);
                /* Motion is computed from time: a late tick takes a bigger step. A jump is
                 * a step larger than vmax allows over the time between the two samples
                 * (plus 2 urad of integer rounding), as the bench judges speed. */
                const int64_t excess = d - (VMAX * dt) / 1000000000 - 2;
                if (d > max_step) {
                    max_step = d;
                    at_dt = dt;
                }
                if (excess > max_excess) {
                    max_excess = excess;
                }
            }
        }
    }
    printf("info largest per-cycle step across the switch %lld urad over %.3f ms; largest step beyond vmax x dt "
           "%lld urad\n", (long long)max_step, (double)at_dt / 1e6, (long long)max_excess);
    CHECK(max_step > 0);
    CHECK(max_excess <= 0);           /* no jump: the switch starts from where the arm actually is */
}

static void fr_a4_lease_expires_without_renewal_and_brakes(void)
{
    const uint64_t exp0 = tm_counter(&g_tm->lease_expiries);
    arb_telem_sample_t s0, s1;
    uint64_t i0, istop;

    CHECK_EQ_INT(move_to(-1000000, 0, 2000), ARB_ARM_OK);   /* a long move, then silence */
    i0 = tm_head();
    CHECK(tm_get(i0 - 1u, &s0));
    CHECK(wait_state(ARM_STATE_BRAKED, 500, 0));
    istop = first_change(i0, ARM_STATE_MOVING, &s1);
    CHECK(istop != UINT64_MAX);
    if (istop != UINT64_MAX) {
        printf("info no renewal: braking began %.2f ms after the setpoint (lease %d ms)\n",
               cyc_to_ms(s1.t_cycles - s0.t_cycles), ARB_LEASE_MS);
        CHECK(cyc_to_ms(s1.t_cycles - s0.t_cycles) < (double)ARB_LEASE_MS + 5.0);
        CHECK_EQ_INT(s1.lease_ok, 0);
    }
    CHECK(tm_counter(&g_tm->lease_expiries) == exp0 + 1u);
}

static void fr_a4_renewal_after_expiry_does_not_resume(void)
{
    arb_telem_sample_t s;

    renew_for(g_coid, 50);
    CHECK(tm_last(&s));
    CHECK_EQ_INT(s.state, ARM_STATE_BRAKED);          /* still latched */
    CHECK_EQ_INT(move_to(0, 0, 2000), ARB_ARM_OK);     /* only a new setpoint moves it */
    CHECK(wait_state(ARM_STATE_IDLE, 3000, 1));
}

static void fr_a4_renewal_from_another_process_ignored(void)
{
    const uint64_t exp0 = tm_counter(&g_tm->lease_expiries);
    pid_t child;
    int status = 0;

    CHECK_EQ_INT(move_to(1000000, 0, 2000), ARB_ARM_OK);
    child = fork();
    if (child == 0) {
        const int coid = name_open(ARB_ARM_NAME, 0);
        if (coid == -1) {
            _exit(10);
        }
        renew_for(coid, 300);                          /* renewals from the wrong connection */
        _exit(0);
    }
    CHECK(child > 0);
    CHECK(wait_state(ARM_STATE_BRAKED, 500, 0));
    (void)waitpid(child, &status, 0);
    CHECK(tm_counter(&g_tm->lease_expiries) == exp0 + 1u);
}

static void fr_a4_guardian_disconnect_brakes_at_once(void)
{
    const uint64_t d0 = tm_counter(&g_tm->disconnects);
    arb_telem_sample_t s0, s1;
    uint64_t i0, istop;

    CHECK_EQ_INT(move_to(-1000000, 0, 2000), ARB_ARM_OK);
    renew_for(g_coid, 200);                            /* moving, lease kept alive */
    i0 = tm_head();
    CHECK(tm_get(i0 - 1u, &s0));
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_close.html */
    (void)name_close(g_coid);                          /* the "guardian" goes away */
    g_coid = -1;
    CHECK(wait_state(ARM_STATE_BRAKED, 500, 0));
    istop = first_change(i0, ARM_STATE_MOVING, &s1);
    CHECK(istop != UINT64_MAX);
    if (istop != UINT64_MAX) {
        printf("info disconnect: braking began %.2f ms after the connection closed (lease would take %d ms)\n",
               cyc_to_ms(s1.t_cycles - s0.t_cycles), ARB_LEASE_MS);
        CHECK(cyc_to_ms(s1.t_cycles - s0.t_cycles) < (double)ARB_LEASE_MS);
    }
    CHECK(tm_counter(&g_tm->disconnects) == d0 + 1u);
    CHECK(connect_arm() >= 0);                         /* a new guardian can bind again */
    {
        arb_arm_reply_t r;
        CHECK_EQ_INT(send_state(g_coid, &r), EOK);
    }
}

static void fr_g5_hold_pulse_stops_without_latching(void)
{
    CHECK_EQ_INT(move_to(1000000, 0, 2000), ARB_ARM_OK);
    renew_for(g_coid, 100);
    (void)MsgSendPulse(g_coid, ARB_PRIO_ARM, ARB_PULSE_ARM_HOLD, 0);
    CHECK(wait_state(ARM_STATE_IDLE, 500, 1));         /* stopped, not BRAKED */
    CHECK_EQ_INT(move_to(0, 0, 2000), ARB_ARM_OK);
}

static void fr_g5_brake_pulse_latches(void)
{
    renew_for(g_coid, 100);
    (void)MsgSendPulse(g_coid, ARB_PRIO_ARM, ARB_PULSE_ARM_BRAKE, 0);
    CHECK(wait_state(ARM_STATE_BRAKED, 500, 1));       /* latched even while renewals arrive */
}

/* FR-G5: only the bound guardian may reset the arm; another process's reset
 * message is refused with EPERM like its setpoints (FR-G6). */
static void fr_g5_arm_reset_only_from_bound_guardian(void)
{
    const uint64_t rej0 = tm_counter(&g_tm->rejected);
    pid_t child;
    int status = 0;

    child = fork();
    if (child == 0) {
        arb_arm_reply_t r;
        const int coid = name_open(ARB_ARM_NAME, 0);
        if (coid == -1) {
            _exit(10);
        }
        _exit(send_reset(coid, &r) == -1 && errno == EPERM ? 0 : 11);
    }
    CHECK(child > 0);
    (void)waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(tm_counter(&g_tm->rejected) > rej0);
}

/* FR-W6 on the arm alone (and the arm-side latch of judge-review M1-J1): the
 * watchdog is stopped, so its beats stop. The arm must brake by itself, count
 * the loss with cause BEATS, and then refuse every setpoint from its bound
 * guardian, also after the watchdog runs again. Must be the last case: it
 * leaves the arm latched. */
static void fr_w6_arm_refuses_setpoints_after_losing_the_watchdog(void)
{
    arb_telem_sample_t s0, s1;

    CHECK(g_watchdog > 0);
    CHECK_EQ_INT(move_to(-300000, 0, 1000), ARB_ARM_OK);   /* control: setpoints work before */
    renew_for(g_coid, 100);
    CHECK_EQ_INT(tm_counter(&g_tm->wd_lost), 0);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/k/kill.html */
    CHECK(kill(g_watchdog, SIGSTOP) == 0);
    CHECK(wait_state(ARM_STATE_BRAKED, 200, 1));       /* brakes although the "guardian" keeps renewing */
    CHECK_EQ_INT(tm_counter(&g_tm->wd_lost), 1);
    CHECK_EQ_INT(tm_counter(&g_tm->wd_lost_cause), ARB_WD_LOST_BEATS);
    CHECK(tm_last(&s0));
    CHECK_EQ_INT(move_to(-600000, 0, 1000), ARB_ARM_REJECTED);
    CHECK(kill(g_watchdog, SIGCONT) == 0);             /* the watchdog runs again ... */
    ms_sleep(200);
    CHECK_EQ_INT(move_to(-600000, 0, 1000), ARB_ARM_REJECTED);   /* ... and still nothing moves */
    ms_sleep(50);
    CHECK(tm_last(&s1));
    CHECK_EQ_INT(s1.state, ARM_STATE_BRAKED);
    CHECK_EQ_INT(s1.segment, s0.segment);
    CHECK_EQ_INT(s1.pos_urad[0], s0.pos_urad[0]);
}

/* FR-G5: after the watchdog runs again (previous case), a reset from the bound
 * guardian re-registers the arm with it and clears the latch: setpoints work
 * again, and the arm stays supervised (no new watchdog loss). */
static void fr_g5_arm_reset_from_its_guardian_clears_the_watchdog_latch(void)
{
    arb_arm_reply_t r;

    CHECK_EQ_INT(send_reset(g_coid, &r), EOK);
    CHECK_EQ_INT(r.status, ARB_ARM_OK);
    CHECK_EQ_INT(move_to(-600000, 0, 1000), ARB_ARM_OK);
    renew_for(g_coid, 100);
    CHECK_EQ_INT(tm_counter(&g_tm->wd_lost), 1);          /* only the loss of the previous case */
    {
        arb_telem_sample_t s;
        CHECK(tm_last(&s));
        CHECK_EQ_INT(s.state, ARM_STATE_MOVING);
    }
}

/* FR-A1 (the loop is driven by the arm's own QNX timer), judge-review M1-J6:
 * tick-coded pulses from any other connection, even the bound guardian's, do
 * not run the control loop. Each loop run publishes one telemetry sample, so
 * the telemetry head may advance only with real time; every forged tick is
 * counted as rejected. */
static void fr_a1_forged_tick_pulses_do_not_drive_the_loop(void)
{
    const uint64_t rej0 = tm_counter(&g_tm->rejected);
    const uint64_t h0 = tm_head();
    struct timespec t0, t1;
    double elapsed_ms;
    int sent = 0;

    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 2000; i++) {                       /* bound: 2000 pulses */
        if (MsgSendPulse(g_coid, 10, ARB_PULSE_ARM_TICK, 0) == 0) {
            sent++;
        }
    }
    ms_sleep(50);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("info forged ticks: %d sent; telemetry +%llu samples in %.1f ms; rejected +%llu\n", sent,
           (unsigned long long)(tm_head() - h0), elapsed_ms,
           (unsigned long long)(tm_counter(&g_tm->rejected) - rej0));
    CHECK(sent > 0);
    CHECK((double)(tm_head() - h0) <= elapsed_ms + 3.0);
    CHECK_EQ_INT(tm_counter(&g_tm->rejected) - rej0, sent);
}

/* NFR-I6, judge-review M1-J16: a message too short to hold a type is refused
 * with EBADMSG before any field is read, even from the bound guardian, and the
 * arm keeps running. */
static void nfr_i6_arm_refuses_a_message_too_short_to_hold_a_type(void)
{
    const uint64_t rej0 = tm_counter(&g_tm->rejected);
    const uint8_t one = 0x42;
    uint8_t reply[64];
    long rc;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html */
    rc = MsgSend(g_coid, &one, sizeof one, reply, sizeof reply);
    CHECK(rc == -1 && errno == EBADMSG);
    CHECK_EQ_INT(tm_counter(&g_tm->rejected) - rej0, 1);
    {
        const uint64_t h0 = tm_head();
        ms_sleep(20);
        CHECK(tm_head() > h0 + 10);                       /* the loop is still running */
    }
}

static const arb_test_case_t cases[] = {
    ARB_TEST_CASE(fr_g6_arm_binds_to_the_first_guardian_connection),
    ARB_TEST_CASE(fr_a6_telemetry_published_every_cycle),
    ARB_TEST_CASE(fr_a1_control_loop_period_measured),
    ARB_TEST_CASE(fr_a1_forged_tick_pulses_do_not_drive_the_loop),
    ARB_TEST_CASE(nfr_i6_arm_refuses_a_message_too_short_to_hold_a_type),
    ARB_TEST_CASE(fr_g6_arm_rejects_setpoints_from_another_process),
    ARB_TEST_CASE(fr_a3_arm_enforces_vmax_itself_and_moves_in_a_straight_line),
    ARB_TEST_CASE(fr_a3_arm_rejects_targets_beyond_joint_limits),
    ARB_TEST_CASE(fr_a3_stale_start_rejected),
    ARB_TEST_CASE(fr_a3_new_setpoint_mid_move_starts_from_actual_position),
    ARB_TEST_CASE(fr_a4_lease_expires_without_renewal_and_brakes),
    ARB_TEST_CASE(fr_a4_renewal_after_expiry_does_not_resume),
    ARB_TEST_CASE(fr_a4_renewal_from_another_process_ignored),
    ARB_TEST_CASE(fr_a4_guardian_disconnect_brakes_at_once),
    ARB_TEST_CASE(fr_g5_hold_pulse_stops_without_latching),
    ARB_TEST_CASE(fr_g5_brake_pulse_latches),
    ARB_TEST_CASE(fr_g5_arm_reset_only_from_bound_guardian),
    ARB_TEST_CASE(fr_w6_arm_refuses_setpoints_after_losing_the_watchdog),   /* leaves the arm latched ... */
    ARB_TEST_CASE(fr_g5_arm_reset_from_its_guardian_clears_the_watchdog_latch),   /* ... and this clears it */
};

int main(int argc, char **argv)
{
    struct sched_param sp;
    int fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    g_watchdog = argc == 2 ? (pid_t)strtol(argv[1], NULL, 10) : 0;   /* test_arm <watchdog-pid> */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = TEST_PRIO;
    (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    /* Read-only view of the arm's telemetry.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
    fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    if (fd == -1) {
        printf("FAIL cannot open telemetry %s: %s\n", ARB_TELEM_NAME, strerror(errno));
        return 1;
    }
    g_tm = mmap(NULL, sizeof(arb_telem_t), PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (g_tm == MAP_FAILED) {
        printf("FAIL cannot map telemetry: %s\n", strerror(errno));
        return 1;
    }
    return arb_test_run("test_arm", cases, (int)(sizeof cases / sizeof cases[0]));
}
