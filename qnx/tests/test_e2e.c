/*
 * End-to-end tests with the real guardian and the real arm (run by
 * qnx/tests/arm_suite.sh). The client sends commands through /dev/arbiter only,
 * and reads the arm's telemetry to see what actually happened.
 *
 *   test_e2e move                   an approved command moves the arm; silence holds it
 *   test_e2e killed  <guardian-pid> the guardian is killed mid-move: the arm brakes
 *   test_e2e stopped <guardian-pid> the guardian is stopped mid-move: the lease expires
 *   test_e2e armkilled <arm-pid>    the arm is killed: the watchdog puts the guardian in SAFE_STOP
 *
 * QEMU proves function; the times printed are QEMU measurements, not claims.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/neutrino.h>

#include "arb_arm_model.h"
#include "arb_contract.h"
#include "arb_devctl.h"
#include "arb_telemetry.h"
#include "arb_watchdog_proto.h"
#include "arm_motion.h"
#include "arb_test.h"

static const arb_telem_t *g_tm;
static const arb_wd_status_t *g_wd;
static int g_fd = -1;
static uint32_t g_seq = 1000;
static pid_t g_guardian;

static void ms_sleep(long ms)
{
    const struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static uint64_t tm_head(void)
{
    return atomic_load_explicit(&((arb_telem_t *)g_tm)->head, memory_order_acquire);
}

static int tm_last(arb_telem_sample_t *out)
{
    const uint64_t h = tm_head();

    if (h == 0) {
        return 0;
    }
    *out = g_tm->s[(h - 1u) % ARB_TELEM_SLOTS];
    return out->index == h - 1u;
}

static double ms_between(uint64_t c0, uint64_t c1)
{
    return (double)(c1 - c0) * 1000.0 / (double)g_tm->cycles_per_sec;
}

static int submit(int32_t q0, int32_t q1, uint32_t ms, arb_verdict_wire_t *w)
{
    arb_submit_t u;
    int rc;

    memset(&u, 0, sizeof u);
    u.cmd.version = ARB_PROTO_VERSION;
    u.cmd.seq = ++g_seq;
    u.cmd.target_urad[0] = q0;
    u.cmd.target_urad[1] = q1;
    u.cmd.duration_ms = ms;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html */
    rc = devctl(g_fd, ARB_DCMD_SUBMIT, &u, sizeof u, NULL);
    *w = u.verdict;
    if (rc != EOK || w->verdict != ARB_VERDICT_ALLOW) {
        printf("info devctl rc %d verdict %u rule %u state %u\n", rc, (unsigned)w->verdict,
               (unsigned)w->rule, (unsigned)w->state);
    }
    return rc;
}

/* Wait until the latest sample is in `state`; returns its ClockCycles stamp or 0. */
static uint64_t wait_state(uint8_t state, long timeout_ms)
{
    arb_telem_sample_t s;

    for (long i = 0; i < timeout_ms; i++) {
        if (tm_last(&s) && s.state == state) {
            return s.t_cycles;
        }
        ms_sleep(1);
    }
    return 0;
}

/* Wait for the first sample, after now, that is no longer MOVING. */
static uint64_t wait_leaves_moving(long timeout_ms, arb_telem_sample_t *out)
{
    for (long i = 0; i < timeout_ms * 10; i++) {
        if (tm_last(out) && out->state != ARM_STATE_MOVING) {
            return out->t_cycles;
        }
        {
            const struct timespec d = { 0, 100000L };   /* 0.1 ms */
            nanosleep(&d, NULL);
        }
    }
    return 0;
}

/* --------------------------------------------------------------- move mode */

static void fr_a3_guardian_approved_command_moves_the_arm(void)
{
    arb_verdict_wire_t w;
    arb_telem_sample_t s;
    int reached = 0;

    /* The guardian bound itself to the arm at startup, before any command. */
    CHECK(atomic_load(&((arb_telem_t *)g_tm)->bound_pid) > 0);

    /* 0.2 rad in 200 ms: done before the 250 ms silence timeout (a planner that
     * goes quiet for longer is put in HOLD; see fr_g5_guardian_silence_holds_the_arm). */
    CHECK_EQ_INT(submit(-200000, 100000, 200, &w), EOK);
    CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);
    for (int i = 0; i < 600 && !reached; i++) {      /* the guardian renews the lease meanwhile */
        ms_sleep(1);
        reached = tm_last(&s) && s.pos_urad[0] == -200000 && s.pos_urad[1] == 100000;
        if (reached) {
            CHECK(s.lease_ok);
        }
    }
    CHECK(reached);
}

static void fr_g5_guardian_silence_holds_the_arm(void)
{
    /* A 3 s move, then no commands: after the 250 ms silence timeout the guardian
     * enters HOLD and the arm stops along its path, unlatched. */
    arb_verdict_wire_t w;
    arb_telem_sample_t s;
    uint64_t t_cmd, t_idle;

    CHECK_EQ_INT(submit(-2000000, 100000, 3000, &w), EOK);
    CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);
    t_cmd = ClockCycles();
    CHECK(wait_state(ARM_STATE_MOVING, 100) != 0);
    t_idle = wait_state(ARM_STATE_IDLE, 1500);
    CHECK(t_idle != 0);
    if (t_idle != 0) {
        printf("info silence: the arm was stopped %.1f ms after the last command "
               "(timeout 250 ms + braking)\n", ms_between(t_cmd, t_idle));
        CHECK(ms_between(t_cmd, t_idle) > 250.0 && ms_between(t_cmd, t_idle) < 600.0);
    }
    CHECK(tm_last(&s));
    CHECK(s.pos_urad[0] > -2000000);                  /* stopped short of the target */
    CHECK(s.lease_ok);                                /* HOLD is authorized: lease still renewed */
}

/* ------------------------------------------------------------ fault modes */

static void guardian_lost_mid_move(int sig, const char *what, double limit_ms)
{
    arb_verdict_wire_t w;
    arb_telem_sample_t s;
    uint64_t t_fault, t_stop;
    struct timespec ts;

    /* Toward -2 rad: away from the operator's hand area (toward +2 rad the elbow
     * would sweep through it, and the guardian denies that move). */
    CHECK_EQ_INT(submit(-2000000, 0, 3000, &w), EOK);
    CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);
    ms_sleep(150);                                    /* moving, lease renewed, within the silence timeout */
    CHECK(tm_last(&s) && s.state == ARM_STATE_MOVING);
    t_fault = ClockCycles();
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/k/kill.html */
    CHECK(kill(g_guardian, sig) == 0);
    t_stop = wait_leaves_moving(500, &s);
    CHECK(t_stop != 0);
    if (t_stop != 0) {
        printf("info guardian %s: the arm began braking %.2f ms later\n", what, ms_between(t_fault, t_stop));
        CHECK(ms_between(t_fault, t_stop) < limit_ms);
        CHECK(s.state == ARM_STATE_STOPPING || s.state == ARM_STATE_BRAKED);
    }
    CHECK(wait_state(ARM_STATE_BRAKED, 500) != 0);    /* stopped and latched */
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
}

static uint32_t wd_cause(void) { return atomic_load((_Atomic uint32_t *)&g_wd->last_cause); }
static uint32_t wd_role(void)  { return atomic_load((_Atomic uint32_t *)&g_wd->last_role); }

static void fr_a4_guardian_killed_arm_brakes(void)
{
    guardian_lost_mid_move(SIGKILL, "killed", (double)ARB_LEASE_MS + 5.0);
}

static void fr_w3_watchdog_records_that_the_guardian_died(void)
{
    ms_sleep(50);
    CHECK_EQ_INT(wd_role(), ARB_WD_ROLE_GUARDIAN);
    CHECK_EQ_INT(wd_cause(), ARB_WD_CAUSE_DIED);
    CHECK(atomic_load((_Atomic uint64_t *)&g_wd->death_notices) >= 1u);
}

static void fr_a4_lease_expires_when_guardian_stopped(void)
{
    guardian_lost_mid_move(SIGSTOP, "stopped", (double)ARB_LEASE_MS + 5.0);
}

static void fr_w2_watchdog_records_a_stuck_guardian_and_brakes_the_arm(void)
{
    ms_sleep(50);
    CHECK_EQ_INT(wd_role(), ARB_WD_ROLE_GUARDIAN);
    CHECK_EQ_INT(wd_cause(), ARB_WD_CAUSE_STUCK);      /* alive but silent: stuck, not dead */
    CHECK(atomic_load((_Atomic uint64_t *)&g_tm->wd_brakes) >= 1u);
    (void)kill(g_guardian, SIGCONT);
}

static void fr_w2_arm_death_puts_the_guardian_in_safe_stop(void)
{
    arb_status_t st;
    arb_verdict_wire_t w;

    CHECK(kill(g_guardian, SIGKILL) == 0);              /* here g_guardian holds the arm's pid */
    ms_sleep(100);
    CHECK_EQ_INT(wd_role(), ARB_WD_ROLE_ARM);
    CHECK(wd_cause() == ARB_WD_CAUSE_DIED);
    memset(&st, 0, sizeof st);
    CHECK_EQ_INT(devctl(g_fd, ARB_DCMD_GET_STATUS, &st, sizeof st, NULL), EOK);
    CHECK_EQ_INT(st.state, ARB_STATE_SAFE_STOP);
    CHECK_EQ_INT(st.stop_cause, ARB_STOP_WATCHDOG);
    CHECK_EQ_INT(st.watchdog_trips, 1);
    CHECK_EQ_INT(submit(-100000, 0, 1000, &w), EOK);    /* nothing moves until a reset */
    CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_STATE);
}

static const arb_test_case_t move_cases[] = {
    ARB_TEST_CASE(fr_a3_guardian_approved_command_moves_the_arm),
    ARB_TEST_CASE(fr_g5_guardian_silence_holds_the_arm),
};
static const arb_test_case_t killed_cases[] = {
    ARB_TEST_CASE(fr_a4_guardian_killed_arm_brakes),
    ARB_TEST_CASE(fr_w3_watchdog_records_that_the_guardian_died),
};
static const arb_test_case_t stopped_cases[] = {
    ARB_TEST_CASE(fr_a4_lease_expires_when_guardian_stopped),
    ARB_TEST_CASE(fr_w2_watchdog_records_a_stuck_guardian_and_brakes_the_arm),
};
static const arb_test_case_t armkilled_cases[] = { ARB_TEST_CASE(fr_w2_arm_death_puts_the_guardian_in_safe_stop) };

int main(int argc, char **argv)
{
    const char *mode = argc >= 2 ? argv[1] : "";
    int fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    if (fd == -1) {
        printf("FAIL cannot open telemetry: %s\n", strerror(errno));
        return 1;
    }
    g_tm = mmap(NULL, sizeof(arb_telem_t), PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    fd = shm_open(ARB_WD_STATUS_NAME, O_RDONLY, 0);
    if (fd == -1) {
        printf("FAIL cannot open watchdog status: %s\n", strerror(errno));
        return 1;
    }
    g_wd = mmap(NULL, sizeof(arb_wd_status_t), PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    g_fd = open(ARB_DEV_PATH, O_RDWR);
    if (g_tm == MAP_FAILED || g_wd == MAP_FAILED || g_fd < 0) {
        printf("FAIL cannot map telemetry or open %s\n", ARB_DEV_PATH);
        return 1;
    }
    if (strcmp(mode, "move") == 0) {
        return arb_test_run("test_e2e move", move_cases, 2);
    }
    if (argc == 3) {
        g_guardian = (pid_t)strtol(argv[2], NULL, 10);
        if (strcmp(mode, "killed") == 0) {
            return arb_test_run("test_e2e killed", killed_cases, 2);
        }
        if (strcmp(mode, "stopped") == 0) {
            return arb_test_run("test_e2e stopped", stopped_cases, 2);
        }
        if (strcmp(mode, "armkilled") == 0) {
            return arb_test_run("test_e2e armkilled", armkilled_cases, 1);
        }
    }
    printf("usage: test_e2e move | killed <pid> | stopped <pid>\n");
    return 2;
}
