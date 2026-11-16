/*
 * arb-watchdog: supervises the guardian and the arm controller (FR-W1 to FR-W3).
 *
 * Two failure kinds, two mechanisms (docs/DESIGN_M1_ADDENDUM.md section 2):
 *   died   - a process-manager death notice (registered with MsgRegisterEvent,
 *            as 8.0 requires), delivered through the watchdog's own connection
 *            and accepted only with that connection's scoid (a forged pulse
 *            arrives with the forger's); a disconnect pulse while the process
 *            is still exiting trips at once and the notice refines the cause;
 *   stuck  - alive, but no heartbeat pulse for ARB_WD_TIMEOUT_MS.
 * On a trip it records the cause in a read-only status record and delivers the
 * other client's registered event (arb_watchdog_proto.h): a dead or stuck
 * guardian brakes the arm; a dead or stuck arm puts the guardian in SAFE_STOP.
 * It never sends a message to a client, so no client can block it.
 * FR-W6: every ARB_WD_BEAT_PERIOD_MS it also delivers each registered client's
 * beat event, so a client notices when the watchdog itself stops.
 *
 *   arb-watchdog [guardian-uid arm-uid]   (defaults ARB_UID_GUARDIAN, ARB_UID_ARM; tests
 *                                          that play a role as root pass 0)
 *
 * Each role is accepted only from that role's user ID (ConnectClientInfo()).
 * Exit status: 3 scheduling, 5 setup, 8 privilege drop failed.
 * Privileges (NFR-I5): started as root; once its name and the death notices are
 * set up it switches to user ARB_UID_WATCHDOG for good, keeping only PRIORITY up
 * to its own 66 (arb_privdrop.h). It still delivers the arm's events at 70:
 * measured on QEMU with the security policy, which allows the watchdog only
 * priorities 1-66, every FR-W2 and FR-W5 delivery to the arm worked
 * (evidence/runs/2026-11-15-qemu_secpol/run-210754.log), so it does not need PRIORITY up to 70.
 *
 * FIFO 66: above the guardian, so a guardian spinning in a loop cannot starve it;
 * below only the arm's control loop (owner decision 2026-11-13, arb_prio.h).
 *
 * Logs go to slogger2 (arb_log.h, NFR-Q6): startup, registrations, trips with
 * their cause, refusals and delivery errors, written by the loop after each
 * message or pulse is handled (after its reply).
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
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/syspage.h>

#include "arb_ids.h"
#include "arb_log.h"
#include "arb_own_pulse.h"
#include "arb_prio.h"
#include "arb_privdrop.h"
#include "arb_pulses.h"
#include "arb_watchdog_proto.h"

#define TICK_NS     1000000L
#define TIMEOUT_NS  ((int64_t)ARB_WD_TIMEOUT_MS * 1000000)
#define ROLES       3                    /* index 1 guardian, 2 arm; 0 unused */
#define ARB_RECV_ERRORS_MAX 100              /* consecutive MsgReceive() failures before exiting (M1-J18) */

typedef struct {
    int             registered;
    int             scoid;               /* its connection to us */
    pid_t           pid;
    rcvid_t         rcvid;               /* of the registration message, for MsgDeliverEvent */
    struct sigevent event;               /* its registered trip event */
    struct sigevent beat;                /* its registered beat event (FR-W6) */
    int64_t         last_hb_ns;
} client_t;

/* One owner: the watchdog's single thread. */
static client_t         g_client[ROLES];
static pid_t            g_last_pid[ROLES];   /* last process that held each role */
static int              g_self_scoid = -1;   /* our own connection, learned before the name exists */
static uint32_t         g_ticks;             /* own ticks handled, for the beat period */
static arb_wd_status_t *g_st;
static uid_t            g_role_uid[ROLES];      /* the user ID each role must register from */

/* What the system log last said (NFR-Q6). Owner: the watchdog's single thread. */
static struct {
    int64_t  pid[ROLES];
    uint64_t trips, rejected, deliver_errors;
    uint32_t last_cause;
} g_logged;

static int64_t now_ns(void)
{
    struct timespec ts;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html */
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

static void count(_Atomic uint64_t *c)
{
    atomic_fetch_add_explicit(c, 1u, memory_order_relaxed);
}

static uint32_t other(uint32_t role)
{
    return role == ARB_WD_ROLE_GUARDIAN ? (uint32_t)ARB_WD_ROLE_ARM : (uint32_t)ARB_WD_ROLE_GUARDIAN;
}

/* Record the failure, stop watching that client, and tell the other one. */
static void trip(uint32_t role, uint32_t cause)
{
    client_t *c = &g_client[role];
    client_t *o = &g_client[other(role)];

    if (!c->registered) {
        return;                          /* already handled (death notice and disconnect both arrive) */
    }
    c->registered = 0;
    atomic_store_explicit(&g_st->last_role, role, memory_order_relaxed);
    atomic_store_explicit(&g_st->last_cause, cause, memory_order_relaxed);
    atomic_store_explicit(&g_st->last_pid, (int64_t)c->pid, memory_order_relaxed);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockcycles.html */
    atomic_store_explicit(&g_st->last_cycles, ClockCycles(), memory_order_relaxed);
    atomic_store_explicit(&g_st->pid[role], (int64_t)-1, memory_order_relaxed);
    count(&g_st->trips);
    if (o->registered) {
        /* Non-blocking; only the client that registered this event can receive it.
         * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgdeliverevent.html */
        if (MsgDeliverEvent(o->rcvid, &o->event) == -1) {
            count(&g_st->deliver_errors);
        } else {
            count(&g_st->delivered);
        }
    }
}

/* ------------------------------------------------------------- messages */

static void reply(rcvid_t rcvid, uint16_t status)
{
    arb_wd_reply_t r;

    memset(&r, 0, sizeof r);
    r.version = (uint16_t)ARB_WD_PROTO_VERSION;
    r.status = status;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreply.html */
    (void)MsgReply(rcvid, EOK, &r, sizeof r);
}

static int pid_alive(pid_t pid)
{
    /* "If sig is zero, no signal is sent, but the pid is still checked for validity."
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/k/kill.html */
    return kill(pid, 0) == 0 || errno != ESRCH;
}

static void handle_register(rcvid_t rcvid, const struct _msg_info *info, const arb_wd_register_t *m, int64_t now)
{
    struct _client_info ci;
    client_t *c;

    if (info->srcmsglen != (ssize64_t)sizeof *m || m->version != ARB_WD_PROTO_VERSION ||
        (m->role != ARB_WD_ROLE_GUARDIAN && m->role != ARB_WD_ROLE_ARM)) {
        count(&g_st->rejected);
        reply(rcvid, EBADMSG);
        return;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectclientinfo.html */
    if (ConnectClientInfo(info->scoid, &ci, 0) == -1 || ci.cred.euid != g_role_uid[m->role]) {
        count(&g_st->rejected);
        reply(rcvid, EPERM);
        return;
    }
    c = &g_client[m->role];
    if (c->registered && pid_alive(c->pid) && c->pid != ci.pid) {
        count(&g_st->rejected);
        reply(rcvid, EBUSY);             /* the role is held by another live process: no hijacking */
        return;
    }
    /* The holder itself may register again (FR-G5 operator reset): its new
     * registration replaces the old one, connection and events included. */
    memset(c, 0, sizeof *c);
    c->registered = 1;
    c->scoid = info->scoid;
    c->pid = ci.pid;
    c->rcvid = rcvid;
    c->event = m->event;
    c->beat = m->beat;
    c->last_hb_ns = now;
    g_last_pid[m->role] = ci.pid;
    atomic_store_explicit(&g_st->pid[m->role], (int64_t)ci.pid, memory_order_relaxed);
    reply(rcvid, 0);                     /* the rcvid stays usable for MsgDeliverEvent */
}

/* --------------------------------------------------------------- pulses */

static void handle_pulse(const struct _pulse *p, int64_t now)
{
    switch (p->code) {
    case ARB_PULSE_WD_TICK:
        if (p->scoid != g_self_scoid) {
            count(&g_st->rejected);                     /* not from our timer */
            return;
        }
        for (uint32_t r = 1; r < ROLES; r++) {         /* bound: ROLES */
            if (g_client[r].registered && now - g_client[r].last_hb_ns > TIMEOUT_NS) {
                trip(r, ARB_WD_CAUSE_STUCK);
            }
        }
        /* FR-W6: our own beat to each client still registered. Non-blocking
         * ("Blocking states: None"); a client the watchdog tripped gets no more
         * beats, so it also stops on its own when it runs again.
         * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgdeliverevent.html */
        if (++g_ticks % (uint32_t)ARB_WD_BEAT_PERIOD_MS == 0u) {
            for (uint32_t r = 1; r < ROLES; r++) {     /* bound: ROLES */
                if (!g_client[r].registered) {
                    continue;
                }
                if (MsgDeliverEvent(g_client[r].rcvid, &g_client[r].beat) == -1) {
                    count(&g_st->beat_errors);
                } else {
                    count(&g_st->beats);
                }
            }
        }
        return;
    case ARB_PULSE_WD_HEARTBEAT: {
        const long role = p->value.sival_long;
        if (role >= 1 && role < ROLES && g_client[role].registered && g_client[role].scoid == p->scoid) {
            g_client[role].last_hb_ns = now;
            count(&g_st->heartbeats[role]);
            atomic_store_explicit(&g_st->last_hb_cycles[role], ClockCycles(), memory_order_relaxed);
        } else {
            count(&g_st->rejected);     /* not from that role's connection */
        }
        return;
    }
    case ARB_PULSE_WD_DEATH: {
        const pid_t pid = (pid_t)p->value.sival_int;   /* the dying process's pid */
        count(&g_st->death_pulses);
        atomic_store_explicit(&g_st->last_death_value, (int64_t)pid, memory_order_relaxed);
        if (p->scoid != g_self_scoid) {
            count(&g_st->rejected);     /* not delivered by the process manager: forged */
            return;
        }
        for (uint32_t r = 1; r < ROLES; r++) {         /* bound: ROLES */
            if (g_last_pid[r] != pid || pid <= 0) {
                continue;
            }
            count(&g_st->death_notices);
            if (g_client[r].registered && g_client[r].pid == pid) {
                trip(r, ARB_WD_CAUSE_DIED);
            } else if (atomic_load_explicit(&g_st->last_pid, memory_order_relaxed) == (int64_t)pid &&
                       atomic_load_explicit(&g_st->last_cause, memory_order_relaxed) == ARB_WD_CAUSE_DISCONNECTED) {
                /* The disconnect arrived first and already tripped; the notice says why. */
                atomic_store_explicit(&g_st->last_cause, (uint32_t)ARB_WD_CAUSE_DIED, memory_order_relaxed);
            }
        }
        return;
    }
    case _PULSE_CODE_DISCONNECT:
        for (uint32_t r = 1; r < ROLES; r++) {         /* bound: ROLES */
            if (g_client[r].registered && g_client[r].scoid == p->scoid) {
                /* Trip now; if the process is dying, its death notice follows and
                 * refines the recorded cause to DIED. */
                trip(r, ARB_WD_CAUSE_DISCONNECTED);
            }
        }
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectdetach.html */
        (void)ConnectDetach(p->scoid);
        return;
    default:
        count(&g_st->rejected);
        return;
    }
}

/* ------------------------------------------------------------- system log */

static const char *role_text(uint32_t role)
{
    return role == ARB_WD_ROLE_GUARDIAN ? "guardian" : role == ARB_WD_ROLE_ARM ? "arm" : "unknown role";
}

static const char *cause_text(uint32_t cause)
{
    switch (cause) {
    case ARB_WD_CAUSE_DIED:         return "died (death notice)";
    case ARB_WD_CAUSE_STUCK:        return "stuck (heartbeats missing)";
    case ARB_WD_CAUSE_DISCONNECTED: return "disconnected";
    default:                        return "unknown cause";
    }
}

static uint64_t st_count(_Atomic uint64_t *c)
{
    return atomic_load_explicit(c, memory_order_relaxed);
}

/* NFR-Q6: write what changed since the last call. Called by the loop after each
 * message or pulse has been handled. Bounded: ROLES + 4 comparisons, at most
 * seven bounded slog2 writes. Two trips in one pass (both clients at once) are
 * logged as one line with the last one's cause and the trip count. */
static void log_changes(void)
{
    const uint64_t trips = st_count(&g_st->trips);
    const uint32_t cause = atomic_load_explicit(&g_st->last_cause, memory_order_relaxed);
    const uint64_t rejected = st_count(&g_st->rejected);
    const uint64_t deliver_errors = st_count(&g_st->deliver_errors);

    for (uint32_t r = 1; r < ROLES; r++) {             /* bound: ROLES */
        const int64_t pid = atomic_load_explicit(&g_st->pid[r], memory_order_relaxed);
        if (pid != g_logged.pid[r] && pid != -1) {
            arb_log_si(ARB_LOG_CLIENT, SLOG2_INFO, "registered the %s, pid %lld", role_text(r), pid);
        }
        g_logged.pid[r] = pid;
    }
    if (trips != g_logged.trips) {
        const uint32_t role = atomic_load_explicit(&g_st->last_role, memory_order_relaxed);
        arb_log_ssi(ARB_LOG_TRIP, SLOG2_WARNING, "trip: the %s %s, pid %lld; the other client was told",
                    role_text(role), cause_text(cause),
                    atomic_load_explicit(&g_st->last_pid, memory_order_relaxed));
    } else if (cause != g_logged.last_cause && trips != 0u) {
        arb_log_ssi(ARB_LOG_TRIP, SLOG2_WARNING, "trip cause refined: the %s %s, pid %lld",
                    role_text(atomic_load_explicit(&g_st->last_role, memory_order_relaxed)), cause_text(cause),
                    atomic_load_explicit(&g_st->last_pid, memory_order_relaxed));
    }
    g_logged.trips = trips;
    g_logged.last_cause = cause;
    if (arb_log_worth(g_logged.rejected, rejected)) {
        arb_log_1(ARB_LOG_REFUSED, SLOG2_WARNING,
                  "refused a registration, message or pulse not from its rightful source (%lld so far)",
                  (int64_t)rejected);
    }
    g_logged.rejected = rejected;
    if (arb_log_worth(g_logged.deliver_errors, deliver_errors)) {
        arb_log_1(ARB_LOG_FAULT, SLOG2_ERROR, "could not deliver a trip event (%lld so far)",
                  (int64_t)deliver_errors);
    }
    g_logged.deliver_errors = deliver_errors;
}

/* ------------------------------------------------------------------ setup */

static int status_create(void)
{
    int fd;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
    (void)shm_unlink(ARB_WD_STATUS_NAME);
    fd = shm_open(ARB_WD_STATUS_NAME, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd == -1) {
        return -1;
    }
    if (ftruncate(fd, (off_t)sizeof(arb_wd_status_t)) == -1) {
        (void)close(fd);
        return -1;
    }
    g_st = mmap(NULL, sizeof(arb_wd_status_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (g_st == MAP_FAILED) {
        g_st = NULL;
        return -1;
    }
    memset(g_st, 0, sizeof *g_st);
    g_st->magic = ARB_WD_STATUS_MAGIC;
    g_st->version = ARB_WD_PROTO_VERSION;
    g_st->cycles_per_sec = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    for (int r = 0; r < ROLES; r++) {                   /* bound: ROLES */
        atomic_store_explicit(&g_st->pid[r], (int64_t)-1, memory_order_relaxed);
    }
    return 0;
}

int main(int argc, char **argv)
{
    union {
        struct _pulse     pulse;
        uint16_t          type;
        arb_wd_register_t reg;            /* the largest message; longer ones fail the size check */
    } msg;
    struct _msg_info info;
    struct sched_param sp;
    struct sigevent death_ev, tick_ev;
    struct itimerspec its;
    name_attach_t *attach;
    dispatch_t *dpp;
    timer_t timer;
    int chid, self_coid, rc;
    int recv_errors = 0;

    (void)arb_log_init("arb_watchdog", "arb-watchdog");   /* first: startup failures are logged too */
    if (argc != 1 && argc != 3) {
        fprintf(stderr, "usage: arb-watchdog [guardian-uid arm-uid]\n");
        return 5;
    }
    g_role_uid[ARB_WD_ROLE_GUARDIAN] = argc == 3 ? (uid_t)strtoul(argv[1], NULL, 10) : ARB_UID_GUARDIAN;
    g_role_uid[ARB_WD_ROLE_ARM] = argc == 3 ? (uid_t)strtoul(argv[2], NULL, 10) : ARB_UID_ARM;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedparam.html */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = ARB_PRIO_WATCHDOG;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        arb_log_fail("set FIFO priority", rc);
        return 3;
    }
    if (status_create() != 0) {
        arb_log_fail("create status " ARB_WD_STATUS_NAME, errno);
        return 5;
    }

    /* Own channel (no priority inheritance; disconnect pulses; no _NTO_CHF_UNBLOCK,
     * so a client whose registration times out is released by the kernel),
     * registered by name on our own dispatch handle.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html */
    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT);
    if (chid == -1) {
        arb_log_fail("ChannelCreate", errno);
        return 5;
    }
    dpp = dispatch_create_channel(chid, 0);
    if (dpp == NULL) {
        arb_log_fail("dispatch_create_channel", errno);
        return 5;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html */
    self_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (self_coid == -1) {
        arb_log_fail("ConnectAttach", errno);
        return 5;
    }

    /* Death notices for every process. The event must be registered with the
     * process manager first; UPDATEABLE lets it put the dead pid in the value.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/procmgr_event_notify_add.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_Termination_of_daemons.html */
    SIGEV_PULSE_INIT(&death_ev, self_coid, ARB_PRIO_WATCHDOG, ARB_PULSE_WD_DEATH, 0);
    SIGEV_MAKE_UPDATEABLE(&death_ev);
    if (MsgRegisterEvent(&death_ev, SYSMGR_COID) == -1) {
        arb_log_fail("MsgRegisterEvent", errno);
        return 5;
    }
    if (procmgr_event_notify_add(PROCMGR_EVENT_PROCESS_DEATH, &death_ev) == -1) {
        arb_log_fail("procmgr_event_notify_add", errno);
        return 5;
    }

    /* 1 ms tick for the heartbeat check.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html */
    SIGEV_PULSE_INIT(&tick_ev, self_coid, ARB_PRIO_WATCHDOG, ARB_PULSE_WD_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &tick_ev, &timer) == -1) {
        arb_log_fail("timer_create", errno);
        return 5;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = TICK_NS;
    its.it_interval.tv_nsec = TICK_NS;
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        arb_log_fail("timer_settime", errno);
        return 5;
    }
    /* Learn our own connection's scoid from the first timer pulse, before the name
     * exists. Kernel-delivered pulses on self_coid (the timer, the death notices)
     * carry this scoid; a forged pulse carries the forger's. A process that
     * guessed our channel ID could still send one before the name exists, so the
     * kernel is also asked that the connection is our own (arb_own_pulse.h).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/_pulse.html */
    for (int i = 0; i < 1000 && g_self_scoid == -1; i++) {        /* bound: 1000 receives */
        if (MsgReceive(chid, &msg, sizeof msg, &info) == 0 && msg.pulse.code == ARB_PULSE_WD_TICK) {
            (void)arb_own_pulse(&g_self_scoid, msg.pulse.scoid);
        }
    }
    if (g_self_scoid == -1) {
        arb_log_fail("first timer pulse (none in 1000 receives)", ETIMEDOUT);
        return 5;
    }
    atomic_store_explicit(&g_st->self_scoid, (int64_t)g_self_scoid, memory_order_relaxed);
    attach = name_attach(dpp, ARB_WD_NAME, 0);
    if (attach == NULL || attach->chid != chid) {
        arb_log_fail("name_attach " ARB_WD_NAME " (or it did not keep our channel)", attach == NULL ? errno : EINVAL);
        return 5;
    }
    /* NFR-I5: root is needed no further. CONNECTION is not kept: the watchdog sends
     * no pulse to another user; it only delivers events its clients registered. */
    {
        char report[ARB_PRIVDROP_REPORT_LEN];
        const char *what;
        int err;

        if (arb_privdrop(ARB_UID_WATCHDOG, ARB_PRIO_WATCHDOG, 0, 0, 0, &what, &err, report) != 0) {
            arb_log_fail(what, err);
            return 8;
        }
        printf("arb-watchdog: %s\n", report);
        arb_log_s(ARB_LOG_START, SLOG2_INFO, "%s", report);
    }
    printf("arb-watchdog: ready as %s, priority %d, timeout %d ms, status %s\n", ARB_WD_NAME,
           ARB_PRIO_WATCHDOG, ARB_WD_TIMEOUT_MS, ARB_WD_STATUS_NAME);
    arb_log_1(ARB_LOG_START, SLOG2_INFO, "ready as " ARB_WD_NAME ", heartbeat timeout %lld ms",
              (int64_t)ARB_WD_TIMEOUT_MS);
    for (int r = 0; r < ROLES; r++) {                   /* bound: ROLES */
        g_logged.pid[r] = -1;
    }
    fflush(stdout);

    /* The server loop, for the life of the process.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html */
    for (;;) {
        const rcvid_t rcvid = MsgReceive(chid, &msg, sizeof msg, &info);
        const int64_t now = now_ns();

        if (rcvid == -1) {
            /* M1-J18: do not spin at priority 66 on a receive that keeps failing.
             * Exiting is seen by the arm and the guardian, which stop (FR-W5). */
            if (++recv_errors >= ARB_RECV_ERRORS_MAX) {
                arb_log_fail("MsgReceive, 100 consecutive errors; exiting", errno);
                return 7;
            }
            continue;
        }
        recv_errors = 0;
        if (rcvid == 0) {
            handle_pulse(&msg.pulse, now);
        } else if (info.srcmsglen < (ssize64_t)sizeof msg.type) {
            count(&g_st->rejected);                /* M1-J16: too short to hold a type */
            (void)MsgError(rcvid, EBADMSG);
        } else if (msg.type == _IO_CONNECT) {
            (void)MsgReply(rcvid, EOK, NULL, 0);   /* defensive: not observed from name_open() */
        } else if (msg.type == ARB_WD_MSG_REGISTER) {
            handle_register(rcvid, &info, &msg.reg, now);
        } else {
            count(&g_st->rejected);
            /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html */
            (void)MsgError(rcvid, msg.type <= _IO_MAX ? ENOSYS : EBADMSG);
        }
        log_changes();                 /* after the reply or the pulse, never inside a handler */
    }
}
