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
 *
 *   arb-watchdog [trusted-uid]      (default uid 0 in M1, where all run as root)
 *
 * FIFO 66: above the guardian, so a guardian spinning in a loop cannot starve it;
 * below only the arm's control loop (owner decision 2026-11-13, arb_prio.h).
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

#include "arb_prio.h"
#include "arb_pulses.h"
#include "arb_watchdog_proto.h"

#define TICK_NS     1000000L
#define TIMEOUT_NS  ((int64_t)ARB_WD_TIMEOUT_MS * 1000000)
#define ROLES       3                    /* index 1 guardian, 2 arm; 0 unused */

typedef struct {
    int             registered;
    int             scoid;               /* its connection to us */
    pid_t           pid;
    rcvid_t         rcvid;               /* of the registration message, for MsgDeliverEvent */
    struct sigevent event;               /* its registered trip event */
    int64_t         last_hb_ns;
} client_t;

/* One owner: the watchdog's single thread. */
static client_t         g_client[ROLES];
static pid_t            g_last_pid[ROLES];   /* last process that held each role */
static int              g_self_scoid = -1;   /* our own connection, learned before the name exists */
static arb_wd_status_t *g_st;
static uid_t            g_trusted_uid;

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
    if (ConnectClientInfo(info->scoid, &ci, 0) == -1 || ci.cred.euid != g_trusted_uid) {
        count(&g_st->rejected);
        reply(rcvid, EPERM);
        return;
    }
    c = &g_client[m->role];
    if (c->registered && pid_alive(c->pid)) {
        count(&g_st->rejected);
        reply(rcvid, EBUSY);             /* the role is held by a live process: no hijacking */
        return;
    }
    memset(c, 0, sizeof *c);
    c->registered = 1;
    c->scoid = info->scoid;
    c->pid = ci.pid;
    c->rcvid = rcvid;
    c->event = m->event;
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

    if (argc > 2) {
        fprintf(stderr, "usage: arb-watchdog [trusted-uid]\n");
        return 5;
    }
    g_trusted_uid = argc == 2 ? (uid_t)strtoul(argv[1], NULL, 10) : 0;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedparam.html */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = ARB_PRIO_WATCHDOG;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        fprintf(stderr, "arb-watchdog: cannot set FIFO priority %d: %s\n", ARB_PRIO_WATCHDOG, strerror(rc));
        return 3;
    }
    if (status_create() != 0) {
        fprintf(stderr, "arb-watchdog: cannot create %s: %s\n", ARB_WD_STATUS_NAME, strerror(errno));
        return 5;
    }

    /* Own channel (no priority inheritance; disconnect pulses; no _NTO_CHF_UNBLOCK,
     * so a client whose registration times out is released by the kernel),
     * registered by name on our own dispatch handle.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html */
    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT);
    if (chid == -1) {
        return 5;
    }
    dpp = dispatch_create_channel(chid, 0);
    if (dpp == NULL) {
        return 5;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html */
    self_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (self_coid == -1) {
        return 5;
    }

    /* Death notices for every process. The event must be registered with the
     * process manager first; UPDATEABLE lets it put the dead pid in the value.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/procmgr_event_notify_add.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_Termination_of_daemons.html */
    SIGEV_PULSE_INIT(&death_ev, self_coid, ARB_PRIO_WATCHDOG, ARB_PULSE_WD_DEATH, 0);
    SIGEV_MAKE_UPDATEABLE(&death_ev);
    if (MsgRegisterEvent(&death_ev, SYSMGR_COID) == -1) {
        fprintf(stderr, "arb-watchdog: MsgRegisterEvent: %s\n", strerror(errno));
        return 5;
    }
    if (procmgr_event_notify_add(PROCMGR_EVENT_PROCESS_DEATH, &death_ev) == -1) {
        fprintf(stderr, "arb-watchdog: procmgr_event_notify_add: %s\n", strerror(errno));
        return 5;
    }

    /* 1 ms tick for the heartbeat check.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html */
    SIGEV_PULSE_INIT(&tick_ev, self_coid, ARB_PRIO_WATCHDOG, ARB_PULSE_WD_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &tick_ev, &timer) == -1) {
        return 5;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = TICK_NS;
    its.it_interval.tv_nsec = TICK_NS;
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        return 5;
    }
    /* Learn our own connection's scoid from the first timer pulse, before the name
     * exists, so no other process can have sent it. Kernel-delivered pulses on
     * self_coid (the timer, the death notices) carry this scoid; a forged pulse
     * carries the forger's.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/_pulse.html */
    for (int i = 0; i < 1000 && g_self_scoid == -1; i++) {        /* bound: 1000 receives */
        if (MsgReceive(chid, &msg, sizeof msg, &info) == 0 && msg.pulse.code == ARB_PULSE_WD_TICK) {
            g_self_scoid = msg.pulse.scoid;
        }
    }
    if (g_self_scoid == -1) {
        fprintf(stderr, "arb-watchdog: no timer pulse received; not starting\n");
        return 5;
    }
    atomic_store_explicit(&g_st->self_scoid, (int64_t)g_self_scoid, memory_order_relaxed);
    attach = name_attach(dpp, ARB_WD_NAME, 0);
    if (attach == NULL || attach->chid != chid) {
        fprintf(stderr, "arb-watchdog: name_attach %s failed\n", ARB_WD_NAME);
        return 5;
    }
    printf("arb-watchdog: ready as %s, priority %d, timeout %d ms, status %s\n", ARB_WD_NAME,
           ARB_PRIO_WATCHDOG, ARB_WD_TIMEOUT_MS, ARB_WD_STATUS_NAME);
    fflush(stdout);

    /* The server loop, for the life of the process.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html */
    for (;;) {
        const rcvid_t rcvid = MsgReceive(chid, &msg, sizeof msg, &info);
        const int64_t now = now_ns();

        if (rcvid == -1) {
            continue;
        }
        if (rcvid == 0) {
            handle_pulse(&msg.pulse, now);
        } else if (msg.type == _IO_CONNECT) {
            (void)MsgReply(rcvid, EOK, NULL, 0);   /* defensive: not observed from name_open() */
        } else if (msg.type == ARB_WD_MSG_REGISTER) {
            handle_register(rcvid, &info, &msg.reg, now);
        } else {
            count(&g_st->rejected);
            /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html */
            (void)MsgError(rcvid, msg.type <= _IO_MAX ? ENOSYS : EBADMSG);
        }
    }
}
