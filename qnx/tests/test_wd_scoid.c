/*
 * On-target test: the arm and the guardian accept the watchdog's beat and trip
 * pulses only from the connection they registered with, whatever value a pulse
 * carries (midpoint review M2; FR-W6 beats, FR-W2 trips).
 *
 *   test_wd_scoid <contract.bin>
 *
 * A forked child plays the watchdog: it serves ARB_WD_NAME as the watchdog's
 * user, so the real arm and guardian (started here) register with it, and it
 * keeps the two registrations, so it knows each client's token. It delivers
 * genuine beats with MsgDeliverEvent(), as arb-watchdog does. The parent then
 * forges pulses with the right code and the right token on connections of its
 * own (name_open() of the arm, open() of /dev/arbiter) and checks that every one
 * is refused and counted, and that a watchdog that falls silent is still noticed
 * while its beat is forged.
 *
 * Genuine trips from the real watchdog are covered by the arm_suite cases that
 * kill or stop a process (test_e2e killed, stopped, armkilled).
 *
 * QEMU proves function; the times printed are QEMU measurements, not claims.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/wait.h>

#include "arb_arm_proto.h"
#include "arb_contract.h"
#include "arb_devctl.h"
#include "arb_ids.h"
#include "arb_prio.h"
#include "arb_privdrop.h"
#include "arb_pulses.h"
#include "arb_telemetry.h"
#include "arb_watchdog_proto.h"
#include "arb_test.h"

#define FORGED_N        200     /* forged pulses per kind and per client */
#define STANDIN_MAX_MS  120000  /* the stand-in serves at most this long (bound) */

/* Shared between the parent and the stand-in watchdog (anonymous shared memory). */
typedef struct {
    _Atomic int      ready;            /* the stand-in serves ARB_WD_NAME */
    _Atomic int      stop;             /* parent -> stand-in: exit */
    _Atomic int      beating;          /* parent -> stand-in: deliver beats */
    _Atomic int      registered[3];    /* [role] */
    _Atomic uint64_t token[3];         /* [role]: value carried by the client's events */
    _Atomic uint64_t beats;            /* beats delivered */
    _Atomic uint64_t beat_errors;
    _Atomic int      standin_error;    /* errno of a failed setup step, 0 if none */
} ctl_t;

static ctl_t *g_ctl;
static const arb_telem_t *g_tm;
static int g_dev = -1;                 /* the parent's own connection to the guardian */
static int g_arm = -1;                 /* the parent's own connection to the arm */
static pid_t g_standin = -1, g_arm_pid = -1, g_guard_pid = -1;

static void ms_sleep(long ms)
{
    const struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static int64_t now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static uint64_t ld(const _Atomic uint64_t *a) { return atomic_load((_Atomic uint64_t *)a); }

static int get_status(arb_status_t *st)
{
    memset(st, 0, sizeof *st);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html */
    return devctl(g_dev, ARB_DCMD_GET_STATUS, st, sizeof *st, NULL);
}

/* ------------------------------------------------------- stand-in watchdog */

typedef union {
    struct _pulse      pulse;
    uint16_t           type;
    arb_wd_register_t  reg;
} standin_msg_t;

static void standin_reply(rcvid_t rcvid, uint16_t status)
{
    arb_wd_reply_t r;

    memset(&r, 0, sizeof r);
    r.version = (uint16_t)ARB_WD_PROTO_VERSION;
    r.status = status;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreply.html */
    (void)MsgReply(rcvid, EOK, &r, sizeof r);
}

/* Serves registrations and delivers each client's beat every ARB_WD_BEAT_PERIOD_MS
 * while ctl->beating is set. Never trips anyone and ignores heartbeats. */
static int standin_main(void)
{
    rcvid_t client[3] = { 0, 0, 0 };
    struct sigevent beat[3];
    name_attach_t *attach;
    int64_t last_beat = 0;

    /* The real watchdog's priority, so its beats are not starved by the clients. */
    {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = ARB_PRIO_WATCHDOG;
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedparam.html */
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != EOK) {
            atomic_store(&g_ctl->standin_error, EPERM);
            return 1;
        }
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html */
    attach = name_attach(NULL, ARB_WD_NAME, 0);
    if (attach == NULL) {
        atomic_store(&g_ctl->standin_error, errno);
        return 1;
    }
    /* The clients check that the server behind the name is the watchdog's user
     * (arb_server_check.h): become that user with the real watchdog's drop. */
    {
        char report[ARB_PRIVDROP_REPORT_LEN];
        const char *what;
        int err;

        if (arb_privdrop(ARB_UID_WATCHDOG, ARB_PRIO_WATCHDOG, 0, 0, 0, &what, &err, report) != 0) {
            atomic_store(&g_ctl->standin_error, err);
            return 1;
        }
    }
    atomic_store(&g_ctl->ready, 1);

    for (int ms = 0; ms < STANDIN_MAX_MS && !atomic_load(&g_ctl->stop); ms++) {   /* bound: STANDIN_MAX_MS receives */
        const uint64_t timeout_ns = 1000000ull;
        struct _msg_info info;
        standin_msg_t msg;
        rcvid_t rcvid;
        int64_t t;

        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html */
        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html */
        rcvid = MsgReceive(attach->chid, &msg, sizeof msg, &info);
        if (rcvid > 0) {
            if (msg.type == _IO_CONNECT) {
                (void)MsgReply(rcvid, EOK, NULL, 0);
            } else if (msg.type == ARB_WD_MSG_REGISTER && info.msglen >= (ssize64_t)sizeof msg.reg &&
                       (msg.reg.role == ARB_WD_ROLE_GUARDIAN || msg.reg.role == ARB_WD_ROLE_ARM)) {
                const uint32_t r = msg.reg.role;
                client[r] = rcvid;
                beat[r] = msg.reg.beat;
                atomic_store(&g_ctl->token[r], (uint64_t)msg.reg.event.sigev_value.sival_long);
                standin_reply(rcvid, 0);              /* the rcvid stays usable for MsgDeliverEvent */
                atomic_store(&g_ctl->registered[r], 1);
            } else {
                /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html */
                (void)MsgError(rcvid, ENOSYS);
            }
        }
        t = now_ns();
        if (atomic_load(&g_ctl->beating) && t - last_beat >= (int64_t)ARB_WD_BEAT_PERIOD_MS * 1000000LL) {
            last_beat = t;
            for (uint32_t r = ARB_WD_ROLE_GUARDIAN; r <= ARB_WD_ROLE_ARM; r++) {   /* bound: 2 roles */
                if (client[r] == 0) {
                    continue;
                }
                /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgdeliverevent.html */
                if (MsgDeliverEvent(client[r], &beat[r]) == -1) {
                    atomic_fetch_add(&g_ctl->beat_errors, 1u);
                } else {
                    atomic_fetch_add(&g_ctl->beats, 1u);
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ setup */

/* Wait up to ms for *flag to become non-zero. */
static int wait_flag(_Atomic int *flag, int ms)
{
    for (int i = 0; i < ms / 10; i++) {               /* bound: ms / 10 polls */
        if (atomic_load(flag)) {
            return 1;
        }
        ms_sleep(10);
    }
    return atomic_load(flag) != 0;
}

/* fork() and exec one Arbiter binary from the deploy directory.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fork.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/e/execv.html */
static pid_t start(char *const argv[])
{
    const pid_t p = fork();

    if (p == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    return p;
}

static void stop_all(void)
{
    if (g_ctl != NULL) {
        atomic_store(&g_ctl->beating, 1);             /* no false alarm while stopping */
    }
    if (g_guard_pid > 0) {
        (void)kill(g_guard_pid, SIGTERM);
        (void)waitpid(g_guard_pid, NULL, 0);
    }
    if (g_arm_pid > 0) {
        (void)kill(g_arm_pid, SIGTERM);
        (void)waitpid(g_arm_pid, NULL, 0);
    }
    if (g_standin > 0) {
        atomic_store(&g_ctl->stop, 1);
        (void)waitpid(g_standin, NULL, 0);
    }
}

/* ------------------------------------------------------------------- tests */

/* Genuine beats, delivered by the process the clients registered with, keep both
 * the arm and the guardian supervised, and none of them is counted as forged. */
static void fr_w6_genuine_beats_from_the_watchdog_connection_are_accepted(void)
{
    const uint64_t rej0 = ld(&g_tm->rejected), lost0 = ld(&g_tm->wd_lost), beats0 = ld(&g_ctl->beats);
    arb_status_t st0, st;

    CHECK_EQ_INT(get_status(&st0), EOK);
    ms_sleep(300);
    CHECK_EQ_INT(get_status(&st), EOK);
    printf("info genuine beats delivered in 300 ms: %llu (errors %llu); arm rejected +%llu, guardian foreign +%u\n",
           (unsigned long long)(ld(&g_ctl->beats) - beats0), (unsigned long long)ld(&g_ctl->beat_errors),
           (unsigned long long)(ld(&g_tm->rejected) - rej0), st.foreign_pulses - st0.foreign_pulses);
    CHECK(ld(&g_ctl->beats) - beats0 >= 100u);           /* ~150 per client pair expected at 2 ms */
    CHECK_EQ_INT(ld(&g_ctl->beat_errors), 0);
    CHECK_EQ_INT(ld(&g_tm->wd_lost) - lost0, 0);
    CHECK_EQ_INT(ld(&g_tm->rejected) - rej0, 0);
    CHECK_EQ_INT(st.watchdog_lost, 0);
    CHECK_EQ_INT(st.foreign_pulses - st0.foreign_pulses, 0);
    CHECK(st.state != ARB_STATE_SAFE_STOP);
}

/* A trip pulse with the right code and the right token, sent on a connection
 * that is not the watchdog's, neither brakes the arm nor stops the guardian. */
static void fr_w2_forged_trip_with_the_right_token_is_refused(void)
{
    const long arm_token = (long)ld(&g_ctl->token[ARB_WD_ROLE_ARM]);
    const long guard_token = (long)ld(&g_ctl->token[ARB_WD_ROLE_GUARDIAN]);
    const uint64_t rej0 = ld(&g_tm->rejected), brakes0 = ld(&g_tm->wd_brakes);
    arb_status_t st0, st;
    int sent = 0;

    CHECK_EQ_INT(get_status(&st0), EOK);
    for (int i = 0; i < FORGED_N; i++) {              /* bound: FORGED_N rounds, ~FORGED_N ms */
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html */
        sent += MsgSendPulse(g_arm, 10, ARB_PULSE_ARM_WD_BRAKE, arm_token) == 0;
        sent += MsgSendPulse(g_dev, 10, ARB_PULSE_GUARDIAN_WD_TRIP, guard_token) == 0;
        ms_sleep(1);
    }
    ms_sleep(20);
    CHECK_EQ_INT(get_status(&st), EOK);
    printf("info forged trips with the right token: %d sent; arm wd_brakes +%llu, rejected +%llu; "
           "guardian trips +%u, foreign +%u, state %u\n", sent,
           (unsigned long long)(ld(&g_tm->wd_brakes) - brakes0), (unsigned long long)(ld(&g_tm->rejected) - rej0),
           st.watchdog_trips - st0.watchdog_trips, st.foreign_pulses - st0.foreign_pulses, (unsigned)st.state);
    CHECK_EQ_INT(sent, 2 * FORGED_N);
    CHECK_EQ_INT(ld(&g_tm->wd_brakes) - brakes0, 0);
    CHECK(ld(&g_tm->rejected) - rej0 >= (uint64_t)FORGED_N);           /* every forged trip to the arm counted */
    CHECK_EQ_INT(st.watchdog_trips - st0.watchdog_trips, 0);
    CHECK(st.foreign_pulses - st0.foreign_pulses >= (uint32_t)FORGED_N); /* and every one to the guardian */
    CHECK(st.state != ARB_STATE_SAFE_STOP);
    CHECK_EQ_INT(st.watchdog_lost, 0);
}

/* The watchdog falls silent while beats with the right code and the right token
 * arrive on another connection every millisecond. Both clients must still decide
 * the watchdog is lost (no beat), and count every forged beat. Leaves the arm
 * latched and the guardian in SAFE_STOP: last. */
static void fr_w6_forged_beat_with_the_right_token_is_refused(void)
{
    const long arm_token = (long)ld(&g_ctl->token[ARB_WD_ROLE_ARM]);
    const long guard_token = (long)ld(&g_ctl->token[ARB_WD_ROLE_GUARDIAN]);
    const uint64_t rej0 = ld(&g_tm->rejected), lost0 = ld(&g_tm->wd_lost);
    arb_status_t st0, st;
    int64_t t0, arm_lost_ns = -1, guard_lost_ns = -1;
    int sent = 0;

    CHECK_EQ_INT(get_status(&st0), EOK);
    CHECK_EQ_INT(st0.watchdog_lost, 0);
    atomic_store(&g_ctl->beating, 0);                  /* the watchdog goes silent */
    t0 = now_ns();
    for (int i = 0; i < FORGED_N; i++) {              /* bound: FORGED_N rounds, ~FORGED_N ms */
        sent += MsgSendPulse(g_arm, 10, ARB_PULSE_ARM_WD_BEAT, arm_token) == 0;
        sent += MsgSendPulse(g_dev, 10, ARB_PULSE_GUARDIAN_WD_BEAT, guard_token) == 0;
        /* The loss must be seen while beats are still being forged: once the
         * forger stops, plain silence would trip the clients whether or not the
         * forged beats were refused. */
        if (arm_lost_ns < 0 && ld(&g_tm->wd_lost) != lost0) {
            arm_lost_ns = now_ns() - t0;
        }
        if (guard_lost_ns < 0 && get_status(&st) == EOK && st.watchdog_lost != 0) {
            guard_lost_ns = now_ns() - t0;
        }
        ms_sleep(1);
    }
    CHECK_EQ_INT(get_status(&st), EOK);
    printf("info forged beats with the right token: %d sent in %.1f ms; arm wd_lost +%llu (cause %llu) seen %.1f ms "
           "after silence, rejected +%llu; guardian lost %u (cause %u) seen %.1f ms after silence, foreign +%u, "
           "state %u (-1.0: not seen while forging)\n", sent, (double)(now_ns() - t0) / 1e6,
           (unsigned long long)(ld(&g_tm->wd_lost) - lost0), (unsigned long long)ld(&g_tm->wd_lost_cause),
           arm_lost_ns < 0 ? -1.0 : (double)arm_lost_ns / 1e6, (unsigned long long)(ld(&g_tm->rejected) - rej0),
           (unsigned)st.watchdog_lost, (unsigned)st.watchdog_lost_cause,
           guard_lost_ns < 0 ? -1.0 : (double)guard_lost_ns / 1e6, st.foreign_pulses - st0.foreign_pulses,
           (unsigned)st.state);
    CHECK_EQ_INT(sent, 2 * FORGED_N);
    CHECK(arm_lost_ns >= 0);                           /* lost while the forger was still sending */
    CHECK(guard_lost_ns >= 0);
    CHECK_EQ_INT(ld(&g_tm->wd_lost) - lost0, 1);
    CHECK_EQ_INT(ld(&g_tm->wd_lost_cause), ARB_WD_LOST_BEATS);
    CHECK(ld(&g_tm->rejected) - rej0 >= (uint64_t)FORGED_N);
    CHECK_EQ_INT(st.watchdog_lost, 1);
    CHECK_EQ_INT(st.watchdog_lost_cause, ARB_WD_LOST_BEATS);
    CHECK(st.foreign_pulses - st0.foreign_pulses >= (uint32_t)FORGED_N);
    CHECK_EQ_INT(st.state, ARB_STATE_SAFE_STOP);
}

static const arb_test_case_t cases[] = {
    ARB_TEST_CASE(fr_w6_genuine_beats_from_the_watchdog_connection_are_accepted),
    ARB_TEST_CASE(fr_w2_forged_trip_with_the_right_token_is_refused),
    ARB_TEST_CASE(fr_w6_forged_beat_with_the_right_token_is_refused),   /* latches: last */
};

int main(int argc, char **argv)
{
    char guard_bin[] = "./arb-guardian", arm_bin[] = "./arb-arm", op_uid[16];
    int fd, rc = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) {
        printf("usage: test_wd_scoid <contract.bin>\n");
        return 2;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
    g_ctl = mmap(NULL, sizeof *g_ctl, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, NOFD, 0);
    if (g_ctl == MAP_FAILED) {
        printf("FAIL cannot map the control block: %s\n", strerror(errno));
        return 1;
    }
    memset(g_ctl, 0, sizeof *g_ctl);
    atomic_store(&g_ctl->beating, 1);
    g_standin = fork();
    if (g_standin == 0) {
        _exit(standin_main());
    }
    if (g_standin < 0 || !wait_flag(&g_ctl->ready, 2000)) {
        printf("FAIL the stand-in watchdog did not start (error %d)\n", atomic_load(&g_ctl->standin_error));
        stop_all();
        return 1;
    }
    {
        char *const argv_arm[] = { arm_bin, argv[1], NULL };
        g_arm_pid = start(argv_arm);
    }
    if (g_arm_pid < 0 || !wait_flag(&g_ctl->registered[ARB_WD_ROLE_ARM], 3000)) {
        printf("FAIL the arm did not register with the stand-in watchdog\n");
        stop_all();
        return 1;
    }
    ms_sleep(200);                                     /* the arm attaches its name after registering */
    (void)snprintf(op_uid, sizeof op_uid, "%d", ARB_UID_OPERATOR);
    {
        char *const argv_guard[] = { guard_bin, argv[1], op_uid, NULL };
        g_guard_pid = start(argv_guard);
    }
    if (g_guard_pid < 0 || !wait_flag(&g_ctl->registered[ARB_WD_ROLE_GUARDIAN], 3000)) {
        printf("FAIL the guardian did not register with the stand-in watchdog\n");
        stop_all();
        return 1;
    }
    ms_sleep(200);
    fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    if (fd != -1) {
        g_tm = mmap(NULL, sizeof(arb_telem_t), PROT_READ, MAP_SHARED, fd, 0);
        (void)close(fd);
    }
    g_dev = open(ARB_DEV_PATH, O_RDONLY);              /* our own connection to the guardian's channel */
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html */
    g_arm = name_open(ARB_ARM_NAME, 0);                /* and to the arm's */
    if (fd == -1 || g_tm == MAP_FAILED || g_dev == -1 || g_arm == -1) {
        printf("FAIL cannot map telemetry, open %s or connect to the arm\n", ARB_DEV_PATH);
    } else {
        printf("info stand-in watchdog pid %d (uid %d); arm pid %d, guardian pid %d\n", (int)g_standin,
               ARB_UID_WATCHDOG, (int)g_arm_pid, (int)g_guard_pid);
        rc = arb_test_run("test_wd_scoid", cases, (int)(sizeof cases / sizeof cases[0]));
    }
    stop_all();
    return rc;
}
