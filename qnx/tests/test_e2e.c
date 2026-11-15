/*
 * End-to-end tests with the real guardian and the real arm (run by
 * qnx/tests/arm_suite.sh). The client sends commands through /dev/arbiter only,
 * and reads the arm's telemetry to see what actually happened.
 *
 *   test_e2e move                   an approved command moves the arm; silence holds it
 *   test_e2e killed  <guardian-pid> the guardian is killed mid-move: the arm brakes
 *   test_e2e stopped <guardian-pid> the guardian is stopped mid-move: the lease expires
 *   test_e2e armkilled <arm-pid>    the arm is killed: the watchdog puts the guardian in SAFE_STOP
 *   test_e2e log <guardian-pid> <arm-pid> <watchdog-pid>
 *                                   NFR-Q6: state changes, a refused sender, a brake and a
 *                                   watchdog trip are in the system log (slog2info)
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
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "arb_arm_model.h"
#include "arb_arm_proto.h"
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
static pid_t g_arm;
static pid_t g_watchdog;

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

/* Judge-review M1-J1: after the watchdog has braked the arm for a stuck guardian,
 * the guardian resumes (SIGCONT, sent by the test above) and a planner sends an
 * approved-looking command. The arm must stay braked: it refuses every setpoint
 * after a genuine watchdog brake, so the guardian reports an arm fault and
 * enters SAFE_STOP, and nothing moves until the processes are restarted. */
static void fr_w2_arm_stays_braked_after_a_stuck_guardian_resumes(void)
{
    arb_verdict_wire_t w;
    arb_telem_sample_t before, after;
    arb_status_t st;

    ms_sleep(100);                                      /* the guardian runs again */
    CHECK(tm_last(&before));
    CHECK_EQ_INT(before.state, ARM_STATE_BRAKED);
    CHECK_EQ_INT(submit(-100000, 0, 1000, &w), EOK);    /* a valid move, away from the hand area */
    CHECK(w.verdict == ARB_VERDICT_DENY);
    CHECK_EQ_INT(w.rule, ARB_RULE_ARM_FAULT);
    ms_sleep(300);
    CHECK(tm_last(&after));
    CHECK_EQ_INT(after.state, ARM_STATE_BRAKED);
    CHECK_EQ_INT(after.segment, before.segment);        /* no new segment was started */
    CHECK_EQ_INT(after.pos_urad[0], before.pos_urad[0]);
    CHECK_EQ_INT(after.pos_urad[1], before.pos_urad[1]);
    memset(&st, 0, sizeof st);
    CHECK_EQ_INT(devctl(g_fd, ARB_DCMD_GET_STATUS, &st, sizeof st, NULL), EOK);
    CHECK_EQ_INT(st.state, ARB_STATE_SAFE_STOP);
}

static int get_status(arb_status_t *st)
{
    memset(st, 0, sizeof *st);
    return devctl(g_fd, ARB_DCMD_GET_STATUS, st, sizeof *st, NULL);
}

static uint64_t wd_heartbeats(uint32_t role)
{
    return atomic_load(&((arb_wd_status_t *)g_wd)->heartbeats[role]);
}

/* FR-G7: the guardian emits a heartbeat every control cycle. Over one second,
 * the guardian's own tick count (counted in its tick-pulse handler) and the
 * heartbeats the watchdog accepted from the guardian's connection must agree
 * tick for tick. The rate (~1 per ms) is a QEMU function check, not timing. */
static void fr_g7_guardian_sends_a_heartbeat_every_tick(void)
{
    arb_status_t s0, s1;
    uint64_t h0, h1;
    long dt, dh;

    CHECK_EQ_INT(get_status(&s0), EOK);
    h0 = wd_heartbeats(ARB_WD_ROLE_GUARDIAN);
    ms_sleep(1000);
    CHECK_EQ_INT(get_status(&s1), EOK);
    h1 = wd_heartbeats(ARB_WD_ROLE_GUARDIAN);
    dt = (long)(s1.ticks - s0.ticks);
    dh = (long)(h1 - h0);
    printf("info guardian in 1 s: %ld ticks, %ld heartbeats accepted by the watchdog (QEMU, function only)\n",
           dt, dh);
    CHECK(dt >= 900 && dt <= 1100);                     /* a 1 ms tick */
    CHECK(dh - dt <= 2 && dt - dh <= 2);                /* one heartbeat per tick (reads are not simultaneous) */
}

/* NFR-Q2, run-time half (the source half is scripts/check-ipc.py): a command is
 * one synchronous request/reply. When devctl() returns, the guardian has already
 * decided it and, for ALLOW, the arm has already accepted the setpoint (the
 * guardian's own MsgSend() to the arm returned OK first): its decision counter
 * has moved by exactly one, the verdict echoes the command's sequence number,
 * and the arm's segment counter shows one new segment per ALLOW. The arm's
 * telemetry is published once per control cycle, so it is read 5 ms later. */
static void nfr_q2_each_command_is_one_synchronous_request_and_reply(void)
{
    arb_verdict_wire_t w;
    arb_telem_sample_t s;
    arb_status_t st0, st1;
    uint32_t seg0;

    ms_sleep(20);
    CHECK(tm_last(&s));
    seg0 = s.segment;
    for (int i = 1; i <= 5; i++) {                      /* bound: 5 commands, under the burst limit */
        int32_t q0 = s.pos_urad[0] - 10000 * i;         /* small steps away from the hand area */

        CHECK_EQ_INT(get_status(&st0), EOK);
        CHECK_EQ_INT(submit(q0, s.pos_urad[1], 100, &w), EOK);
        CHECK_EQ_INT(get_status(&st1), EOK);
        CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);
        CHECK_EQ_INT(w.seq, g_seq);                     /* the reply is to this command */
        CHECK_EQ_INT(st1.decisions - st0.decisions, 1); /* decided before devctl() returned */
        ms_sleep(5);
        {
            arb_telem_sample_t now;
            CHECK(tm_last(&now));
            CHECK_EQ_INT(now.segment - seg0, i);        /* the arm took exactly this setpoint */
        }
        ms_sleep(25);
    }
}

/* ---------------------------------------------------------------- log mode */

#define LOG_TEXT_MAX 65536

/* Every line slog2info prints for one buffer set of one process ("<set>.<pid>"),
 * into buf. Returns the number of lines.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/slog2info.html */
static int read_log(const char *set, pid_t pid, char *buf, size_t cap)
{
    char cmd[96], tag[64], line[1024];
    size_t used = 0;
    int lines = 0;
    FILE *f;

    buf[0] = '\0';
    snprintf(cmd, sizeof cmd, "slog2info -b %s", set);
    snprintf(tag, sizeof tag, "%s.%d ", set, (int)pid);
    f = popen(cmd, "r");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        const size_t n = strlen(line);
        if (strstr(line, tag) == NULL || used + n + 1 > cap) {
            continue;
        }
        printf("info slog2 %s", line);
        memcpy(buf + used, line, n + 1);
        used += n;
        lines++;
    }
    (void)pclose(f);
    return lines;
}

static int count_of(const char *text, const char *what)
{
    int n = 0;

    for (const char *p = strstr(text, what); p != NULL; p = strstr(p + 1, what)) {
        n++;
    }
    return n;
}

static void nfr_q6_trusted_processes_log_through_slog2(void)
{
    static char text[LOG_TEXT_MAX];
    char want[80];
    arb_verdict_wire_t w;
    arb_arm_setpoint_t q;
    arb_arm_reply_t r;
    int coid;

    /* 1. Guardian state changes: HOLD at startup, NORMAL on a command, HOLD after
     *    the 250 ms silence timeout. */
    CHECK_EQ_INT(submit(-100000, 50000, 100, &w), EOK);
    CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);
    ms_sleep(500);

    /* 2. A refused sender: a setpoint straight to the arm, not from the guardian. */
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html */
    coid = name_open(ARB_ARM_NAME, 0);
    CHECK(coid != -1);
    memset(&q, 0, sizeof q);
    q.hdr.type = (uint16_t)ARB_ARM_MSG_SETPOINT;
    q.hdr.version = (uint16_t)ARB_ARM_PROTO_VERSION;
    q.hdr.seq = 1;
    q.target_urad[0] = 300000;
    q.duration_ms = 1000;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html */
    CHECK(MsgSend(coid, &q, sizeof q, &r, sizeof r) == -1 && errno == EPERM);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_close.html */
    (void)name_close(coid);

    /* 3. A brake and a watchdog trip: the guardian is killed. */
    CHECK(kill(g_guardian, SIGKILL) == 0);
    ms_sleep(300);

    /* The guardian's buffer set outlives it (slogger2 page: logs survive a crash). */
    CHECK(read_log("arb_guardian", g_guardian, text, sizeof text) > 0);
    CHECK(count_of(text, "ready on /dev/arbiter, contract sha256 ") == 1);
    CHECK(count_of(text, "state NORMAL") >= 1);
    CHECK(count_of(text, "state HOLD") >= 2);          /* at startup, and after the silence */

    CHECK(read_log("arb_arm", g_arm, text, sizeof text) > 0);
    CHECK(count_of(text, "ready as arbiter_arm, contract sha256 ") == 1);
    snprintf(want, sizeof want, "bound the guardian, pid %d", (int)g_guardian);
    CHECK(count_of(text, want) == 1);
    CHECK(count_of(text, "refused a sender that is not the bound guardian (1 so far)") == 1);
    CHECK(count_of(text, "guardian connection lost: braked") == 1);

    CHECK(read_log("arb_watchdog", g_watchdog, text, sizeof text) > 0);
    CHECK(count_of(text, "ready as ") == 1);
    snprintf(want, sizeof want, "registered the guardian, pid %d", (int)g_guardian);
    CHECK(count_of(text, want) == 1);
    snprintf(want, sizeof want, "registered the arm, pid %d", (int)g_arm);
    CHECK(count_of(text, want) == 1);
    CHECK(count_of(text, "trip: the guardian") == 1);
}

static const arb_test_case_t move_cases[] = {
    ARB_TEST_CASE(fr_a3_guardian_approved_command_moves_the_arm),
    ARB_TEST_CASE(fr_g5_guardian_silence_holds_the_arm),
    ARB_TEST_CASE(fr_g7_guardian_sends_a_heartbeat_every_tick),
    ARB_TEST_CASE(nfr_q2_each_command_is_one_synchronous_request_and_reply),
};
static const arb_test_case_t killed_cases[] = {
    ARB_TEST_CASE(fr_a4_guardian_killed_arm_brakes),
    ARB_TEST_CASE(fr_w3_watchdog_records_that_the_guardian_died),
};
static const arb_test_case_t stopped_cases[] = {
    ARB_TEST_CASE(fr_a4_lease_expires_when_guardian_stopped),
    ARB_TEST_CASE(fr_w2_watchdog_records_a_stuck_guardian_and_brakes_the_arm),
    ARB_TEST_CASE(fr_w2_arm_stays_braked_after_a_stuck_guardian_resumes),
};
static const arb_test_case_t armkilled_cases[] = { ARB_TEST_CASE(fr_w2_arm_death_puts_the_guardian_in_safe_stop) };
static const arb_test_case_t log_cases[] = { ARB_TEST_CASE(nfr_q6_trusted_processes_log_through_slog2) };

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
        return arb_test_run("test_e2e move", move_cases, 4);
    }
    if (strcmp(mode, "log") == 0 && argc == 5) {
        g_guardian = (pid_t)strtol(argv[2], NULL, 10);
        g_arm = (pid_t)strtol(argv[3], NULL, 10);
        g_watchdog = (pid_t)strtol(argv[4], NULL, 10);
        return arb_test_run("test_e2e log", log_cases, 1);
    }
    if (argc == 3) {
        g_guardian = (pid_t)strtol(argv[2], NULL, 10);
        if (strcmp(mode, "killed") == 0) {
            return arb_test_run("test_e2e killed", killed_cases, 2);
        }
        if (strcmp(mode, "stopped") == 0) {
            return arb_test_run("test_e2e stopped", stopped_cases, 3);
        }
        if (strcmp(mode, "armkilled") == 0) {
            return arb_test_run("test_e2e armkilled", armkilled_cases, 1);
        }
    }
    printf("usage: test_e2e move | killed <pid> | stopped <pid> | armkilled <pid> | log <guardian> <arm> <watchdog>\n");
    return 2;
}
