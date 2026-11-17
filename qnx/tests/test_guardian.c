/*
 * On-target tests of arb-guardian through /dev/arbiter, as an untrusted client
 * would use it. Run by qnx/tests/guardian_suite.sh, which starts the stub arm and
 * the guardian in each mode:
 *
 *   test_guardian normal    the guardian with a well-behaved arm
 *   test_guardian hang      the arm receives setpoints and never replies
 *   test_guardian stopped   the arm process is stopped (never receives)
 *   test_guardian stale     the arm rejects every setpoint as STALE_START
 *
 * Contract: contracts/default.yaml (silence 250 ms, rate 50/s burst 10,
 * threshold 3 in 1 s, operator's hand area x 200..400 mm, y 150..350 mm).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <sys/syspage.h>
#include <sys/wait.h>

#include "arb_as_gateway.h"
#include "arb_contract.h"
#include "arb_decisions.h"
#include "arb_ids.h"
#include "arb_devctl.h"
#include "arb_pulses.h"
#include "arb_test.h"

static int g_fd = -1;          /* opened read-write */
static uint32_t g_seq = 100;

static arb_cmd_t cmd(int32_t q0, int32_t q1, uint32_t ms)
{
    arb_cmd_t c;

    memset(&c, 0, sizeof c);
    c.version = ARB_PROTO_VERSION;
    c.seq = ++g_seq;
    c.target_urad[0] = q0;
    c.target_urad[1] = q1;
    c.duration_ms = ms;
    return c;
}

/* Submit one command; returns devctl()'s result (an errno value) and the verdict. */
static int submit(int fd, const arb_cmd_t *c, arb_verdict_wire_t *w)
{
    arb_submit_t u;
    int rc;

    memset(&u, 0, sizeof u);
    u.cmd = *c;
    /* As the gateway's user (FR-G10).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html */
    rc = arb_devctl_gw(fd, ARB_DCMD_SUBMIT, &u, sizeof u);
    *w = u.verdict;
    return rc;
}

static int status(arb_status_t *s)
{
    memset(s, 0, sizeof *s);
    return devctl(g_fd, ARB_DCMD_GET_STATUS, s, sizeof *s, NULL);
}

static void ms_sleep(long ms)
{
    const struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static int open_rw(void)
{
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html */
    g_fd = open(ARB_DEV_PATH, O_RDWR);
    return g_fd;
}

/* -------------------------------------------------------------- normal mode */

static void fr_g1_dev_arbiter_exists_in_path_namespace(void)
{
    struct stat st;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/stat.html */
    CHECK(stat(ARB_DEV_PATH, &st) == 0);
    CHECK(S_ISNAM(st.st_mode));
    CHECK(g_fd >= 0);
}

static void fr_g1_command_round_trip_over_devctl(void)
{
    const arb_cmd_t c = cmd(-100000, 0, 1000);   /* small move away from the hand area */
    arb_verdict_wire_t w;

    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK_EQ_INT(w.version, ARB_PROTO_VERSION);
    CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);
    CHECK_EQ_INT(w.rule, ARB_RULE_NONE);
    CHECK_EQ_INT(w.seq, c.seq);
    CHECK_EQ_INT(w.target_urad[0], -100000);
    CHECK_EQ_INT(w.state, ARB_STATE_NORMAL);
}

/* ------------------------------------------------- decision ring (FR-G8) */

static const arb_dec_ring_t *g_ring;

/* The guardian's decision ring, mapped read-only (the tests run as root). */
static const arb_dec_ring_t *ring(void)
{
    if (g_ring == NULL) {
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
         * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
        const int fd = shm_open(ARB_DEC_NAME, O_RDONLY, 0);
        if (fd != -1) {
            const void *p = mmap(NULL, sizeof(arb_dec_ring_t), PROT_READ, MAP_SHARED, fd, 0);
            (void)close(fd);
            g_ring = p == MAP_FAILED ? NULL : p;
        }
    }
    return g_ring;
}

static uint64_t ring_head(void)
{
    return atomic_load_explicit(&((arb_dec_ring_t *)g_ring)->head, memory_order_acquire);
}

/* Every command the guardian decides is recorded, in order, with the command,
 * the verdict it replied and the time it took; a refused submit (not decided) is
 * not. The latency bound here is a sanity check (QEMU proves function only). */
static void fr_g8_every_decision_is_recorded_with_its_latency(void)
{
    arb_cmd_t c[3];
    arb_verdict_wire_t w[3];
    arb_submit_t u;
    uint64_t h0;

    if (ring() == NULL) {
        CHECK(!"decision ring " ARB_DEC_NAME " not found");
        return;
    }
    CHECK_EQ_INT(g_ring->magic, ARB_DEC_MAGIC);
    CHECK_EQ_INT(g_ring->version, ARB_DEC_VERSION);
    h0 = ring_head();
    c[0] = cmd(-150000, 0, 1000);                   /* allowed */
    c[1] = cmd(-700000, 0, 100);                    /* too fast: clamped */
    c[2] = c[0];                                    /* replayed: denied */
    for (int i = 0; i < 3; i++) {                   /* bound: 3 */
        CHECK_EQ_INT(submit(g_fd, &c[i], &w[i]), EOK);
    }
    CHECK(w[0].verdict == ARB_VERDICT_ALLOW && w[1].verdict == ARB_VERDICT_CLAMP &&
          w[2].verdict == ARB_VERDICT_DENY && w[2].rule == ARB_RULE_SEQUENCE);
    memset(&u, 0, sizeof u);
    u.cmd = cmd(-100000, 0, 1000);
    CHECK_EQ_INT(devctl(g_fd, ARB_DCMD_SUBMIT, &u, sizeof u, NULL), EPERM);   /* root: refused, not decided */
    CHECK_EQ_INT(ring_head() - h0, 3);
    for (int i = 0; i < 3; i++) {                   /* bound: 3 */
        const arb_dec_record_t *r = &g_ring->r[(h0 + (uint64_t)i) % ARB_DEC_SLOTS];
        CHECK_EQ_INT(r->index, h0 + (uint64_t)i);
        CHECK_EQ_INT(r->cmd.seq, c[i].seq);
        CHECK_EQ_INT(r->cmd.target_urad[0], c[i].target_urad[0]);
        CHECK_EQ_INT(r->cmd.duration_ms, c[i].duration_ms);
        CHECK_EQ_INT(r->verdict.verdict, w[i].verdict);
        CHECK_EQ_INT(r->verdict.rule, w[i].rule);
        CHECK_EQ_INT(r->verdict.state_after, w[i].state);
        CHECK_EQ_INT(r->verdict.duration_ms, w[i].duration_ms);
        CHECK(r->latency_cycles > 0 && r->latency_cycles < g_ring->cycles_per_sec);
        if (i > 0) {
            const arb_dec_record_t *q = &g_ring->r[(h0 + (uint64_t)i - 1u) % ARB_DEC_SLOTS];
            CHECK(r->t_ns >= q->t_ns && r->t_cycles >= q->t_cycles);
        }
    }
    /* Back to where the next test starts (the stub arm reports its last setpoint). */
    c[0] = cmd(-100000, 0, 1000);
    CHECK_EQ_INT(submit(g_fd, &c[0], &w[0]), EOK);
    CHECK_EQ_INT(w[0].verdict, ARB_VERDICT_ALLOW);
}

/* NFR-Q4: the latency is measured with the cycle counter, in the units the ring
 * header gives (the kernel's cycles per second from the system page). */
static void nfr_q4_decision_latency_measured_with_the_cycle_counter(void)
{
    uint64_t lo = UINT64_MAX, hi = 0, h;
    int n = 0;

    if (ring() == NULL) {
        CHECK(!"decision ring " ARB_DEC_NAME " not found");
        return;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/syspage_entry.html */
    CHECK_EQ_INT(g_ring->cycles_per_sec, SYSPAGE_ENTRY(qtime)->cycles_per_sec);
    h = ring_head();
    for (uint64_t i = h > 8u ? h - 8u : 0u; i < h; i++) {   /* bound: 8 records */
        const uint64_t c = g_ring->r[i % ARB_DEC_SLOTS].latency_cycles;
        CHECK(c > 0);
        lo = c < lo ? c : lo;
        hi = c > hi ? c : hi;
        n++;
    }
    CHECK(n > 0);
    if (n > 0) {
        printf("info decision latency over the last %d records: %.1f to %.1f us (QEMU, function only)\n", n,
               (double)lo * 1e6 / (double)g_ring->cycles_per_sec, (double)hi * 1e6 / (double)g_ring->cycles_per_sec);
    }
}

/* FR-G8 "without blocking": with no reader at all, the guardian decides more
 * commands than the ring holds, every one is answered, and the ring holds the
 * newest ARB_DEC_SLOTS of them, in order. Replays are used: decided, cheap, and
 * they do not count toward SAFE_STOP. */
static void fr_g8_recording_never_waits_for_a_reader(void)
{
    const uint32_t n = ARB_DEC_SLOTS + 100u;
    arb_cmd_t c = cmd(-100000, 0, 1000);
    arb_verdict_wire_t w;
    uint64_t h0, h1;
    uint32_t ok = 0, holes = 0;

    if (ring() == NULL) {
        CHECK(!"decision ring " ARB_DEC_NAME " not found");
        return;
    }
    (void)submit(g_fd, &c, &w);                     /* c.seq is now consumed */
    h0 = ring_head();
    for (uint32_t i = 0; i < n; i++) {              /* bound: n */
        ok += submit(g_fd, &c, &w) == EOK && w.rule == ARB_RULE_SEQUENCE;
    }
    h1 = ring_head();
    for (uint64_t i = h1 - ARB_DEC_SLOTS; i < h1; i++) {   /* bound: ARB_DEC_SLOTS */
        holes += g_ring->r[i % ARB_DEC_SLOTS].index != i;
    }
    printf("info %u replays answered, ring head +%llu, %u holes in the newest %u slots\n", ok,
           (unsigned long long)(h1 - h0), holes, ARB_DEC_SLOTS);
    CHECK_EQ_INT(ok, n);
    CHECK_EQ_INT(h1 - h0, n);
    CHECK_EQ_INT(holes, 0);
}

/* NFR-I1: the gateway's user cannot open the decision ring at all; the logger's
 * user can read it and not write it. seteuid() as in arb_as_gateway.h. */
static void nfr_i1_gateway_user_cannot_open_the_decision_ring(void)
{
    int gw_ro, gw_errno, logd_ro, logd_rw, logd_errno;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/seteuid.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/setegid.html */
    CHECK(setegid(ARB_UID_GATEWAY) == 0 && seteuid(ARB_UID_GATEWAY) == 0);
    gw_ro = shm_open(ARB_DEC_NAME, O_RDONLY, 0);
    gw_errno = errno;
    CHECK(seteuid(0) == 0 && setegid(0) == 0);
    CHECK(setegid(ARB_UID_LOGD) == 0 && seteuid(ARB_UID_LOGD) == 0);
    logd_ro = shm_open(ARB_DEC_NAME, O_RDONLY, 0);
    logd_rw = shm_open(ARB_DEC_NAME, O_RDWR, 0);
    logd_errno = errno;
    CHECK(seteuid(0) == 0 && setegid(0) == 0);
    printf("info gateway O_RDONLY %d (errno %d); logger O_RDONLY %d, O_RDWR %d (errno %d)\n", gw_ro, gw_errno,
           logd_ro, logd_rw, logd_errno);
    CHECK(gw_ro == -1 && gw_errno == EACCES);
    CHECK(logd_ro >= 0);
    CHECK(logd_rw == -1 && logd_errno == EACCES);
    if (gw_ro >= 0) {
        (void)close(gw_ro);
    }
    if (logd_ro >= 0) {
        (void)close(logd_ro);
    }
    if (logd_rw >= 0) {
        (void)close(logd_rw);
    }
}

static void fr_g3_clamp_and_denials_over_devctl(void)
{
    arb_verdict_wire_t w;
    arb_cmd_t c;

    c = cmd(-600000, 0, 100);                       /* 0.5 rad in 100 ms: too fast at 1 rad/s */
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK(w.verdict == ARB_VERDICT_CLAMP && w.rule == ARB_RULE_VELOCITY);
    CHECK_EQ_INT(w.duration_ms, 500);              /* stretched to exactly the limit */
    c = cmd(2700000, 0, 5000);                      /* beyond the 150 deg shoulder limit */
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_JOINT_LIMIT);
    c = cmd(785398, 0, 5000);                       /* elbow into the operator's hand area */
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_KEEPOUT);
    CHECK_EQ_INT(w.target_urad[0], 0);             /* a DENY carries no setpoint */
}

static void fr_g4_replay_denied_over_devctl(void)
{
    arb_cmd_t c = cmd(-500000, 0, 1000);
    arb_verdict_wire_t w;

    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK(w.verdict == ARB_VERDICT_ALLOW);
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);       /* the same sequence number again */
    CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_SEQUENCE);
}

static void fr_g2_zero_duration_denied_over_devctl(void)
{
    const arb_cmd_t c = cmd(-500000, 0, 0);
    arb_verdict_wire_t w;

    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_DURATION);
}

static void fr_g4_rate_limit_over_devctl(void)
{
    arb_verdict_wire_t w;
    int rate_denied = 0;

    ms_sleep(300);                                  /* refill the bucket */
    for (int i = 0; i < 15; i++) {                  /* burst is 10 */
        const arb_cmd_t c = cmd(-500000, 0, 1000);
        CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
        if (w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_RATE) {
            rate_denied++;
        }
    }
    CHECK(rate_denied >= 4);
}

static void nfr_i6_devctl_wrong_size_rejected(void)
{
    uint8_t buf[64];

    memset(buf, 0, sizeof buf);
    CHECK_EQ_INT(arb_devctl_gw(g_fd, ARB_DCMD_SUBMIT, buf, sizeof(arb_submit_t) - 1u), EBADMSG);
    CHECK_EQ_INT(arb_devctl_gw(g_fd, ARB_DCMD_SUBMIT, buf, sizeof(arb_submit_t) + 1u), EBADMSG);
    CHECK_EQ_INT(arb_devctl_gw(g_fd, ARB_DCMD_SUBMIT, buf, 0), EBADMSG);
    CHECK_EQ_INT(arb_devctl_gw(g_fd, ARB_DCMD_SUBMIT, buf, sizeof buf), EBADMSG);
}

static void nfr_i6_unknown_devctl_command_rejected(void)
{
    arb_submit_t u;

    memset(&u, 0, sizeof u);
    CHECK_EQ_INT(devctl(g_fd, __DIOTF(_DCMD_MISC, 0x7E, arb_submit_t), &u, sizeof u, NULL), ENOTTY);
}

static void nfr_i6_read_only_descriptor_cannot_submit(void)
{
    const arb_cmd_t c = cmd(-500000, 0, 1000);
    arb_verdict_wire_t w;
    const int ro = open(ARB_DEV_PATH, O_RDONLY);

    CHECK(ro >= 0);
    CHECK_EQ_INT(submit(ro, &c, &w), EBADF);
    (void)close(ro);
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

static void nfr_i6_random_payloads_never_crash_the_guardian(void)
{
    uint64_t seed = 0xC0FFEEull;
    arb_status_t before, after;
    int accepted = 0;

    CHECK_EQ_INT(status(&before), EOK);
    for (int i = 0; i < 2000; i++) {
        arb_submit_t u;
        uint8_t *p = (uint8_t *)&u;
        for (size_t k = 0; k < sizeof u; k++) {
            p[k] = (uint8_t)xorshift64(&seed);
        }
        CHECK_EQ_INT(arb_devctl_gw(g_fd, ARB_DCMD_SUBMIT, &u, sizeof u), EOK);
        if (u.verdict.verdict != ARB_VERDICT_DENY) {
            accepted++;
        }
    }
    CHECK_EQ_INT(accepted, 0);
    CHECK_EQ_INT(status(&after), EOK);              /* still alive and answering */
    CHECK(after.decisions >= before.decisions + 2000u);
    CHECK(after.state != ARB_STATE_SAFE_STOP);      /* malformed input is not a violation */
}

static void fr_g5_silence_enters_hold_over_devctl(void)
{
    const arb_cmd_t c = cmd(-400000, 0, 1000);
    arb_verdict_wire_t w;
    arb_status_t s;

    ms_sleep(100);
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK_EQ_INT(w.state, ARB_STATE_NORMAL);
    ms_sleep(400);                                  /* silence timeout is 250 ms */
    CHECK_EQ_INT(status(&s), EOK);
    CHECK_EQ_INT(s.state, ARB_STATE_HOLD);
    CHECK(s.ticks > 300u);                          /* the 1 ms tick is running */
}

static double mono_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* FR-G5 (the guardian keeps time on its own timer), judge-review M1-J6: a client
 * that sends tick-coded or trip-coded pulses on its /dev/arbiter connection
 * neither adds guardian ticks (and with them heartbeats and lease renewals) nor
 * trips it; every such pulse is ignored and counted. */
static void fr_g5_forged_tick_and_trip_pulses_are_ignored_and_counted(void)
{
    arb_status_t a, b;
    double t0, t1;
    int sent = 0;

    CHECK_EQ_INT(status(&a), EOK);
    t0 = mono_ms();
    for (int i = 0; i < 2000; i++) {                       /* bound: 2000 pulses */
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html */
        if (MsgSendPulse(g_fd, 10, i % 10 == 0 ? ARB_PULSE_GUARDIAN_WD_TRIP : ARB_PULSE_GUARDIAN_TICK, 0) == 0) {
            sent++;
        }
    }
    ms_sleep(50);
    t1 = mono_ms();
    CHECK_EQ_INT(status(&b), EOK);
    printf("info forged pulses: %d sent; guardian ticks +%u in %.1f ms; foreign_pulses +%u\n", sent,
           b.ticks - a.ticks, t1 - t0, b.foreign_pulses - a.foreign_pulses);
    CHECK(sent > 0);
    CHECK((double)(b.ticks - a.ticks) <= t1 - t0 + 3.0);   /* only the real 1 ms timer counts */
    CHECK_EQ_INT(b.foreign_pulses - a.foreign_pulses, sent);
    CHECK_EQ_INT(b.watchdog_trips, a.watchdog_trips);
    CHECK(b.state != ARB_STATE_SAFE_STOP);
}

static void fr_g5_last_client_close_enters_hold(void)
{
    const arb_cmd_t c = cmd(-300000, 0, 1000);
    arb_verdict_wire_t w;
    arb_status_t s;

    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK_EQ_INT(w.state, ARB_STATE_NORMAL);
    (void)close(g_fd);                              /* the last client goes away */
    CHECK(open_rw() >= 0);
    CHECK_EQ_INT(status(&s), EOK);
    CHECK_EQ_INT(s.state, ARB_STATE_HOLD);          /* at once, not after the timeout */
    CHECK_EQ_INT(s.clients, 1);
}

/* ------------------------------------------- FR-G10: the guardian's front door */

static double ms_since(uint64_t t0);   /* below, with the faulty-arm cases */

static void fr_g10_only_the_gateway_user_may_submit(void)
{
    /* Root, the operator, the untrusted user and the guardian's own user, all on
     * the same read-write descriptor: the guardian asks the kernel who sent each
     * message, so only the gateway's user gets a verdict. */
    static const uid_t others[] = { 0, ARB_UID_OPERATOR, ARB_UID_UNTRUSTED, ARB_UID_GUARDIAN };
    arb_status_t a, b;
    arb_verdict_wire_t w;
    arb_cmd_t c;

    ms_sleep(250);                                  /* refill the rate bucket */
    CHECK_EQ_INT(status(&a), EOK);
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; i++) {   /* bound: 4 */
        arb_submit_t u;
        memset(&u, 0, sizeof u);
        u.cmd = cmd(-100000, 0, 1000);
        CHECK_EQ_INT(arb_devctl_as(others[i], g_fd, ARB_DCMD_SUBMIT, &u, sizeof u), EPERM);
    }
    CHECK_EQ_INT(status(&b), EOK);
    CHECK_EQ_INT(b.decisions, a.decisions);         /* refused before any decision */
    CHECK_EQ_INT(b.submits_refused - a.submits_refused, 4);
    c = cmd(-100000, 0, 1000);
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);        /* the gateway's user */
    CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);
}

static void fr_g10_a_second_submitter_is_refused_until_the_first_closes(void)
{
    arb_verdict_wire_t w;
    arb_status_t a, b;
    arb_cmd_t c;
    int second;

    ms_sleep(250);
    c = cmd(-100000, 0, 1000);
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);        /* g_fd is the submitter */
    second = open(ARB_DEV_PATH, O_RDWR);
    CHECK(second >= 0);
    CHECK_EQ_INT(status(&a), EOK);
    c = cmd(-100000, 0, 1000);
    CHECK_EQ_INT(submit(second, &c, &w), EBUSY);    /* the same user, another open */
    CHECK_EQ_INT(status(&b), EOK);
    CHECK_EQ_INT(b.submits_refused - a.submits_refused, 1);
    CHECK_EQ_INT(b.decisions, a.decisions);
    (void)close(g_fd);                              /* the first submitter goes away */
    g_fd = second;
    c = cmd(-100000, 0, 1000);
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);     /* now this open is the submitter */
}

static void fr_g10_status_reports_the_last_sequence(void)
{
    const arb_cmd_t c = cmd(-100000, 0, 1000);
    arb_verdict_wire_t w;
    arb_status_t s;

    ms_sleep(250);
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK_EQ_INT(status(&s), EOK);
    CHECK_EQ_INT(s.has_seq, 1);
    CHECK_EQ_INT(s.last_seq, c.seq);
}

static void fr_g10_restarted_client_resumes_after_the_published_sequence(void)
{
    /* A planner (or gateway) that restarts knows nothing of its old numbers: it
     * reads the status and continues after last_seq. One sequence space for the
     * guardian's lifetime, so an old command replayed in a new session is denied. */
    arb_cmd_t c = cmd(-100000, 0, 1000);
    arb_verdict_wire_t w;
    arb_status_t s;

    ms_sleep(250);
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    (void)close(g_fd);                              /* the client goes away ... */
    CHECK(open_rw() >= 0);                          /* ... and a new one starts */
    CHECK_EQ_INT(status(&s), EOK);
    CHECK_EQ_INT(s.has_seq, 1);
    CHECK_EQ_INT(s.last_seq, c.seq);
    c = cmd(-100000, 0, 1000);
    c.seq = s.last_seq;                             /* the old session's last command */
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_SEQUENCE);
    c.seq = s.last_seq + 1u;
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK_EQ_INT(w.verdict, ARB_VERDICT_ALLOW);
    if ((int32_t)(c.seq - g_seq) > 0) {             /* only ever forward: later tests keep working */
        g_seq = c.seq;
    }
}

static void fr_g5_submitter_killed_with_a_status_reader_open_holds_at_once(void)
{
    /* Review M1: with a logger or dashboard holding /dev/arbiter open, the
     * gateway's death must still be noticed at once, not after the 250 ms silence
     * timeout. The child plays the gateway; this process keeps a read-only open. */
    const arb_cmd_t c = cmd(-100000, 0, 2000);
    int ready[2];
    arb_status_t s;
    pid_t child;
    char b = 0;
    uint64_t t0;
    double held_ms = -1.0;

    ms_sleep(250);
    (void)close(g_fd);                              /* release the submitter slot */
    g_fd = open(ARB_DEV_PATH, O_RDONLY);            /* the status reader */
    CHECK(g_fd >= 0);
    CHECK(pipe(ready) == 0);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fork.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/setuid.html */
    child = fork();
    if (child == 0) {
        arb_submit_t u;
        const int fd = open(ARB_DEV_PATH, O_RDWR);
        char ok = 0;

        memset(&u, 0, sizeof u);
        u.cmd = c;
        if (fd != -1 && setuid((uid_t)ARB_UID_GATEWAY) == 0 &&
            devctl(fd, ARB_DCMD_SUBMIT, &u, sizeof u, NULL) == EOK && u.verdict.verdict == ARB_VERDICT_ALLOW) {
            ok = 1;
        }
        (void)write(ready[1], &ok, 1);
        for (;;) {                                  /* bound: killed by the parent */
            (void)pause();
        }
    }
    CHECK(child > 0);
    (void)close(ready[1]);
    CHECK(read(ready[0], &b, 1) == 1 && b == 1);
    (void)close(ready[0]);
    CHECK_EQ_INT(status(&s), EOK);
    CHECK_EQ_INT(s.state, ARB_STATE_NORMAL);
    CHECK_EQ_INT(s.clients, 2);
    t0 = ClockCycles();
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/k/kill.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/w/waitpid.html */
    CHECK(kill(child, SIGKILL) == 0);
    (void)waitpid(child, NULL, 0);
    for (int i = 0; i < 100; i++) {                 /* bound: 100 x 1 ms */
        if (status(&s) == EOK && s.state == ARB_STATE_HOLD) {
            held_ms = ms_since(t0);
            break;
        }
        ms_sleep(1);
    }
    printf("  info submitter killed -> HOLD after %.3f ms (-1: not within 100 ms), clients %u\n",
           held_ms, (unsigned)s.clients);
    CHECK(held_ms >= 0.0 && held_ms < 50.0);        /* far inside the 250 ms silence timeout */
    CHECK_EQ_INT(s.clients, 1);                     /* the reader is still open */
    (void)close(g_fd);
    CHECK(open_rw() >= 0);
}

static void fr_g5_reset_through_the_gateway_is_refused(void)
{
    /* docs/ARCHITECTURE.md: the operator resets locally, never through the gateway. */
    arb_status_t a, b;

    CHECK_EQ_INT(status(&a), EOK);
    CHECK_EQ_INT(arb_devctl_gw(g_fd, ARB_DCMD_RESET, NULL, 0), EPERM);
    CHECK_EQ_INT(status(&b), EOK);
    CHECK_EQ_INT(b.resets_refused - a.resets_refused, 1);
    CHECK_EQ_INT(b.resets, a.resets);
}

/* ------------------------------------------------------------ faulty arms */

static double ms_since(uint64_t t0)
{
    return (double)(ClockCycles() - t0) * 1000.0 / (double)SYSPAGE_ENTRY(qtime)->cycles_per_sec;
}

static void arm_fault_case(void)
{
    const arb_cmd_t c = cmd(-100000, 0, 1000);
    arb_verdict_wire_t w;
    arb_status_t s;
    uint64_t t0;
    double ms;

    t0 = ClockCycles();
    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    ms = ms_since(t0);
    printf("info devctl with a faulty arm returned after %.3f ms (send timeout 2 ms, 1 ms tick)\n", ms);
    CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_ARM_FAULT);
    CHECK_EQ_INT(w.state, ARB_STATE_SAFE_STOP);
    CHECK(ms < 50.0);                               /* bounded: the guardian did not hang */
    CHECK_EQ_INT(status(&s), EOK);
    CHECK_EQ_INT(s.stop_cause, ARB_STOP_ARM_TIMEOUT);
    CHECK_EQ_INT(s.arm_faults, 1);
    {
        const arb_cmd_t next = cmd(-100000, 0, 1000);
        CHECK_EQ_INT(submit(g_fd, &next, &w), EOK);  /* everything denied until a reset */
        CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_STATE);
    }
}

static void fr_g9_arm_never_replies_times_out_and_safe_stops(void) { arm_fault_case(); }
static void fr_g9_arm_not_receiving_times_out_and_safe_stops(void) { arm_fault_case(); }

static void fr_g3_stale_start_reported_as_deny(void)
{
    const arb_cmd_t c = cmd(-100000, 0, 1000);
    arb_verdict_wire_t w;
    arb_status_t s;

    CHECK_EQ_INT(submit(g_fd, &c, &w), EOK);
    CHECK(w.verdict == ARB_VERDICT_DENY && w.rule == ARB_RULE_STALE_START);
    CHECK_EQ_INT(w.target_urad[0], 0);
    CHECK_EQ_INT(w.state, ARB_STATE_HOLD);          /* not accepted, not a fault */
    CHECK_EQ_INT(status(&s), EOK);
    CHECK_EQ_INT(s.arm_faults, 0);
}

/* ---------------------------------------------------------------------- run */

static const arb_test_case_t normal_cases[] = {
    ARB_TEST_CASE(fr_g1_dev_arbiter_exists_in_path_namespace),
    ARB_TEST_CASE(fr_g1_command_round_trip_over_devctl),
    ARB_TEST_CASE(fr_g8_every_decision_is_recorded_with_its_latency),
    ARB_TEST_CASE(nfr_q4_decision_latency_measured_with_the_cycle_counter),
    ARB_TEST_CASE(nfr_i1_gateway_user_cannot_open_the_decision_ring),
    ARB_TEST_CASE(fr_g3_clamp_and_denials_over_devctl),
    ARB_TEST_CASE(fr_g4_replay_denied_over_devctl),
    ARB_TEST_CASE(fr_g2_zero_duration_denied_over_devctl),
    ARB_TEST_CASE(fr_g4_rate_limit_over_devctl),
    ARB_TEST_CASE(nfr_i6_devctl_wrong_size_rejected),
    ARB_TEST_CASE(nfr_i6_unknown_devctl_command_rejected),
    ARB_TEST_CASE(nfr_i6_read_only_descriptor_cannot_submit),
    ARB_TEST_CASE(nfr_i6_random_payloads_never_crash_the_guardian),
    ARB_TEST_CASE(fr_g5_forged_tick_and_trip_pulses_are_ignored_and_counted),
    ARB_TEST_CASE(fr_g5_silence_enters_hold_over_devctl),
    ARB_TEST_CASE(fr_g10_only_the_gateway_user_may_submit),
    ARB_TEST_CASE(fr_g10_a_second_submitter_is_refused_until_the_first_closes),
    ARB_TEST_CASE(fr_g10_status_reports_the_last_sequence),
    ARB_TEST_CASE(fr_g10_restarted_client_resumes_after_the_published_sequence),
    ARB_TEST_CASE(fr_g5_submitter_killed_with_a_status_reader_open_holds_at_once),
    ARB_TEST_CASE(fr_g5_reset_through_the_gateway_is_refused),
    ARB_TEST_CASE(fr_g8_recording_never_waits_for_a_reader),
    ARB_TEST_CASE(fr_g5_last_client_close_enters_hold),
};
static const arb_test_case_t hang_cases[] = { ARB_TEST_CASE(fr_g9_arm_never_replies_times_out_and_safe_stops) };
static const arb_test_case_t stopped_cases[] = { ARB_TEST_CASE(fr_g9_arm_not_receiving_times_out_and_safe_stops) };
static const arb_test_case_t stale_cases[] = { ARB_TEST_CASE(fr_g3_stale_start_reported_as_deny) };

int main(int argc, char **argv)
{
    const char *mode = argc == 2 ? argv[1] : "";

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (open_rw() < 0) {
        printf("FAIL cannot open %s: %s\n", ARB_DEV_PATH, strerror(errno));
        return 1;
    }
    if (strcmp(mode, "normal") == 0) {
        return arb_test_run("test_guardian normal", normal_cases, (int)(sizeof normal_cases / sizeof normal_cases[0]));
    }
    if (strcmp(mode, "hang") == 0) {
        return arb_test_run("test_guardian hang", hang_cases, 1);
    }
    if (strcmp(mode, "stopped") == 0) {
        return arb_test_run("test_guardian stopped", stopped_cases, 1);
    }
    if (strcmp(mode, "stale") == 0) {
        return arb_test_run("test_guardian stale", stale_cases, 1);
    }
    printf("usage: test_guardian normal|hang|stopped|stale\n");
    return 2;
}
