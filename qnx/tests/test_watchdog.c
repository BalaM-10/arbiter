/*
 * On-target tests of arb-watchdog, one test per run against a freshly started
 * watchdog (qnx/tests/watchdog_suite.sh). Forked children play the guardian and
 * the arm; the parent receives the trip events the watchdog delivers and reads
 * the watchdog's read-only status record.
 *
 *   test_watchdog <test-name>
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
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/wait.h>

#include "arb_prio.h"
#include "arb_pulses.h"
#include "arb_watchdog_proto.h"
#include "arb_wd_client.h"
#include "arb_test.h"

#define TRIP_CODE ARB_PULSE_ARM_WD_BRAKE   /* any code works for a test client */

static const arb_wd_status_t *g_st;

static void ms_sleep(long ms)
{
    const struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static uint64_t ld(const _Atomic uint64_t *a) { return atomic_load((_Atomic uint64_t *)a); }

static double ms_between(uint64_t c0, uint64_t c1)
{
    return c1 > c0 ? (double)(c1 - c0) * 1000.0 / (double)g_st->cycles_per_sec : 0.0;
}

/* A client with its own channel, registered in `role`. */
typedef struct {
    int chid;
    int self_coid;
    arb_wd_client_t wd;
} tclient_t;

static int tclient_open(tclient_t *t, uint32_t role)
{
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html */
    t->chid = ChannelCreate(0);
    t->self_coid = ConnectAttach(0, 0, t->chid, _NTO_SIDE_CHANNEL, 0);
    if (t->chid == -1 || t->self_coid == -1) {
        return -1;
    }
    return arb_wd_client_register(&t->wd, role, t->self_coid, 30, TRIP_CODE);
}

/* Heartbeat for ms milliseconds. */
static void beat(tclient_t *t, long ms)
{
    for (long i = 0; i < ms; i++) {
        arb_wd_client_heartbeat(&t->wd);
        ms_sleep(1);
    }
}

/* Wait (heartbeating) up to timeout_ms for our genuine trip event. */
static int wait_trip(tclient_t *t, long timeout_ms)
{
    for (long i = 0; i < timeout_ms; i++) {
        struct _pulse p;
        const uint64_t to = 1000000ull;
        rcvid_t r;

        arb_wd_client_heartbeat(&t->wd);
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html */
        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
        r = MsgReceive(t->chid, &p, sizeof p, NULL);
        if (r == 0 && arb_wd_client_is_trip(&t->wd, &p, TRIP_CODE)) {
            return 1;
        }
    }
    return 0;
}

/* Child: register in `role`, heartbeat for 100 ms, then `fate`:
 * 0 stop heartbeats and sleep (stuck), 1 exit (die). */
static pid_t spawn_role(uint32_t role, int fate)
{
    const pid_t pid = fork();

    if (pid == 0) {
        tclient_t t;
        if (tclient_open(&t, role) != 0) {
            _exit(10);
        }
        beat(&t, 100);
        if (fate == 1) {
            _exit(0);
        }
        for (;;) {
            ms_sleep(1000);              /* alive, silent */
        }
    }
    return pid;
}

static void reap(pid_t pid)
{
    int st;
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, &st, 0);
}

/* ------------------------------------------------------------------ tests */

static void fr_w3_stuck_detected_by_missed_heartbeat(void)
{
    tclient_t me;
    pid_t arm;

    CHECK_EQ_INT(tclient_open(&me, ARB_WD_ROLE_GUARDIAN), 0);
    arm = spawn_role(ARB_WD_ROLE_ARM, 0);
    beat(&me, 20);                                     /* keep our own role alive while waiting */
    CHECK(ld(&g_st->heartbeats[ARB_WD_ROLE_ARM]) > 0);
    CHECK(wait_trip(&me, 1000));                       /* the arm went quiet: guardian is told */
    CHECK_EQ_INT(atomic_load((_Atomic uint32_t *)&g_st->last_role), ARB_WD_ROLE_ARM);
    CHECK_EQ_INT(atomic_load((_Atomic uint32_t *)&g_st->last_cause), ARB_WD_CAUSE_STUCK);
    CHECK_EQ_INT(atomic_load((_Atomic int64_t *)&g_st->last_pid), arm);
    printf("info stuck: tripped %.2f ms after the last heartbeat (timeout %d ms)\n",
           ms_between(ld(&g_st->last_hb_cycles[ARB_WD_ROLE_ARM]), ld(&g_st->last_cycles)), ARB_WD_TIMEOUT_MS);
    reap(arm);
}

static void fr_w3_death_detected_by_death_notice(void)
{
    tclient_t me;
    pid_t arm;
    int st;

    CHECK_EQ_INT(tclient_open(&me, ARB_WD_ROLE_GUARDIAN), 0);
    arm = spawn_role(ARB_WD_ROLE_ARM, 1);
    while (waitpid(arm, &st, WNOHANG) == 0) {          /* reaped: really gone */
        beat(&me, 1);
    }
    CHECK(wait_trip(&me, 1000));
    beat(&me, 20);
    printf("info death pulses %llu, last value %lld (arm pid %d)\n", (unsigned long long)ld(&g_st->death_pulses),
           (long long)atomic_load((_Atomic int64_t *)&g_st->last_death_value), (int)arm);
    CHECK_EQ_INT(atomic_load((_Atomic uint32_t *)&g_st->last_cause), ARB_WD_CAUSE_DIED);
    CHECK(ld(&g_st->death_notices) >= 1u);             /* the process manager's notice arrived */
    printf("info death: tripped %.2f ms after the last heartbeat; death notices %llu\n",
           ms_between(ld(&g_st->last_hb_cycles[ARB_WD_ROLE_ARM]), ld(&g_st->last_cycles)),
           (unsigned long long)ld(&g_st->death_notices));
}

static void fr_w2_missed_guardian_heartbeat_brakes_the_arm(void)
{
    tclient_t me;
    pid_t guardian;

    CHECK_EQ_INT(tclient_open(&me, ARB_WD_ROLE_ARM), 0);
    guardian = spawn_role(ARB_WD_ROLE_GUARDIAN, 0);
    beat(&me, 20);
    CHECK(wait_trip(&me, 1000));                       /* the arm receives its brake event */
    CHECK_EQ_INT(atomic_load((_Atomic uint32_t *)&g_st->last_role), ARB_WD_ROLE_GUARDIAN);
    CHECK_EQ_INT(atomic_load((_Atomic uint32_t *)&g_st->last_cause), ARB_WD_CAUSE_STUCK);
    CHECK(ld(&g_st->delivered) >= 1u);
    reap(guardian);
}

static void fr_w3_forged_death_notice_ignored(void)
{
    tclient_t me;
    pid_t arm;
    int coid;

    CHECK_EQ_INT(tclient_open(&me, ARB_WD_ROLE_GUARDIAN), 0);
    arm = fork();
    if (arm == 0) {                                    /* a healthy arm, heartbeating */
        tclient_t t;
        if (tclient_open(&t, ARB_WD_ROLE_ARM) != 0) {
            _exit(10);
        }
        for (;;) {
            beat(&t, 1000);
        }
    }
    beat(&me, 50);
    coid = name_open(ARB_WD_NAME, 0);
    CHECK(coid >= 0);
    /* Claim the live arm died, with the watchdog's own death code. */
    CHECK(MsgSendPulse(coid, 20, ARB_PULSE_WD_DEATH, (long)arm) == 0);
    CHECK(!wait_trip(&me, 200));                       /* no trip */
    CHECK_EQ_INT(ld(&g_st->trips), 0);
    CHECK(ld(&g_st->rejected) >= 1u);
    reap(arm);
}

static void fr_w2_heartbeats_from_another_connection_ignored(void)
{
    tclient_t me;
    pid_t arm;
    int coid;

    CHECK_EQ_INT(tclient_open(&me, ARB_WD_ROLE_GUARDIAN), 0);
    arm = spawn_role(ARB_WD_ROLE_ARM, 0);              /* goes quiet after 100 ms */
    coid = name_open(ARB_WD_NAME, 0);
    CHECK(coid >= 0);
    for (int i = 0; i < 500; i++) {                    /* someone else keeps "beating" for the arm */
        (void)MsgSendPulse(coid, 20, ARB_PULSE_WD_HEARTBEAT, (long)ARB_WD_ROLE_ARM);
        if (i % 2 == 0) {
            arb_wd_client_heartbeat(&me.wd);
        }
        ms_sleep(1);
        if (ld(&g_st->trips) > 0) {
            break;
        }
    }
    CHECK_EQ_INT(ld(&g_st->trips), 1);                 /* still detected as stuck */
    CHECK_EQ_INT(atomic_load((_Atomic uint32_t *)&g_st->last_cause), ARB_WD_CAUSE_STUCK);
    reap(arm);
}

static void fr_w3_live_role_cannot_be_registered_twice(void)
{
    tclient_t me, again;
    pid_t arm;

    CHECK_EQ_INT(tclient_open(&me, ARB_WD_ROLE_GUARDIAN), 0);
    arm = fork();
    if (arm == 0) {
        tclient_t t;
        if (tclient_open(&t, ARB_WD_ROLE_ARM) != 0) {
            _exit(10);
        }
        for (;;) {
            beat(&t, 1000);
        }
    }
    beat(&me, 50);
    errno = 0;
    CHECK_EQ_INT(tclient_open(&again, ARB_WD_ROLE_ARM), -1);   /* the live arm keeps the role */
    CHECK_EQ_INT(errno, EBUSY);
    reap(arm);
}

typedef struct { const char *name; void (*fn)(void); } named_t;
#define T(fn) { #fn, fn }
static const named_t all[] = {
    T(fr_w3_stuck_detected_by_missed_heartbeat),
    T(fr_w3_death_detected_by_death_notice),
    T(fr_w2_missed_guardian_heartbeat_brakes_the_arm),
    T(fr_w3_forged_death_notice_ignored),
    T(fr_w2_heartbeats_from_another_connection_ignored),
    T(fr_w3_live_role_cannot_be_registered_twice),
};

int main(int argc, char **argv)
{
    int fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    fd = shm_open(ARB_WD_STATUS_NAME, O_RDONLY, 0);
    if (fd == -1) {
        printf("FAIL cannot open %s: %s\n", ARB_WD_STATUS_NAME, strerror(errno));
        return 1;
    }
    g_st = mmap(NULL, sizeof(arb_wd_status_t), PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (g_st == MAP_FAILED) {
        return 1;
    }
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (argc == 2 && strcmp(argv[1], all[i].name) == 0) {
            const arb_test_case_t one[] = { { all[i].name, all[i].fn } };
            return arb_test_run("test_watchdog", one, 1);
        }
    }
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
            printf("%s\n", all[i].name);
        }
        return 0;
    }
    printf("usage: test_watchdog <test-name> | list\n");
    return 2;
}
