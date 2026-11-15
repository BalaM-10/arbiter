/*
 * arb-arm: the arm controller (FR-A1 to FR-A6, FR-G6 receiving side).
 *
 * A 1 kHz control loop, driven by its own timer pulse, moves the simulated arm
 * along the last approved segment (arm_motion.c) and publishes every cycle to a
 * read-only telemetry ring (arb_telemetry.h). Setpoints are accepted only from
 * the guardian's connection; every authorization expires unless the guardian
 * renews it (the motion lease); the guardian's disconnect brakes the arm at once.
 * Design: docs/DESIGN_M1_ADDENDUM.md sections 2, 4 and 5.
 *
 *   arb-arm <contract.bin> [guardian-uid]     (default uid 0 in M1, where all run as root)
 *
 * Exit status: 2 contract refused, 3 scheduling, 5 setup, 6 no watchdog.
 * The watchdog must be running first: the arm registers with it, sends a
 * heartbeat every cycle, and brakes (latched) when the watchdog says the guardian
 * died or is stuck (FR-W2).
 *
 * Sender binding (M1): the first connection whose effective user ID is the
 * guardian's becomes the only authorized one; others are refused and counted.
 * While every process runs as root the user ID alone cannot tell them apart, so
 * the binding is what separates them until the M2 security policy
 * (docs/THREAT_MODEL.md, known gaps).
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
#include <sys/syspage.h>

#include "arb_arm_model.h"
#include "arb_arm_proto.h"
#include "arb_contract.h"
#include "arb_hex.h"
#include "arb_own_pulse.h"
#include "arb_prio.h"
#include "arb_pulses.h"
#include "arb_telemetry.h"
#include "arb_watchdog_proto.h"
#include "arb_wd_client.h"
#include "arm_backend.h"
#include "arm_motion.h"

#define LEASE_NS ((int64_t)ARB_LEASE_MS * 1000000)
#define ARB_RECV_ERRORS_MAX 100      /* consecutive MsgReceive() failures before exiting (M1-J18) */

/* All state has one owner: the arm's single thread. */
static arb_contract_table_t g_contract;
static arm_motion_t         g_motion;
static arm_backend_t        g_backend;
static arb_telem_t         *g_telem;
static int                  g_bound_scoid = -1;     /* the guardian's connection, once bound */
static int                  g_tick_scoid = -1;      /* our own timer's connection (arb_own_pulse.h) */
static uid_t                g_guardian_uid;
static int                  g_lease_valid;
static int64_t              g_lease_until_ns;
static uint32_t             g_segment;
/* Set by a genuine watchdog brake and never cleared in this process: the
 * watchdog no longer supervises the guardian it tripped on, so no setpoint may
 * move the arm again until restart (judge-review M1-J1; an operator reset path
 * is M2, FR-G5). */
static int                  g_wd_tripped;
static arb_wd_client_t      g_wd;

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

/* --------------------------------------------------------------- telemetry */

static void publish(const arm_sample_t *s, int64_t t_ns)
{
    const uint64_t h = atomic_load_explicit(&g_telem->head, memory_order_relaxed);
    arb_telem_sample_t *e = &g_telem->s[h % ARB_TELEM_SLOTS];

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockcycles.html */
    e->t_cycles = ClockCycles();
    e->index = h;
    e->t_ns = t_ns;
    e->pos_urad[0] = s->pos_urad[0];
    e->pos_urad[1] = s->pos_urad[1];
    e->vel_urad_s[0] = s->vel_urad_s[0];
    e->vel_urad_s[1] = s->vel_urad_s[1];
    e->segment = g_segment;
    e->state = s->state;
    e->lease_ok = (uint8_t)g_lease_valid;
    atomic_store_explicit(&g_telem->head, h + 1u, memory_order_release);
}

/* ------------------------------------------------------------ control loop */

static void control_cycle(int64_t now)
{
    arm_sample_t s;

    /* FR-A4: the authorization expires inside the arm unless renewed. */
    if (g_lease_valid && now > g_lease_until_ns) {
        arm_motion_stop(&g_motion, now, 1);
        g_lease_valid = 0;
        count(&g_telem->lease_expiries);
    }
    arm_motion_sample(&g_motion, now, &s);
    g_backend.apply(&g_backend, s.pos_urad, s.vel_urad_s);
    publish(&s, now);
    arb_wd_client_heartbeat(&g_wd);
}

static void brake_latched(int64_t now)
{
    arm_motion_stop(&g_motion, now, 1);
    g_lease_valid = 0;
}

/* ----------------------------------------------------------- authorization */

/* FR-G6: only the guardian's connection may command the arm.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectclientinfo.html */
static int authorized(int scoid, int may_bind)
{
    struct _client_info ci;

    if (g_bound_scoid != -1) {
        return scoid == g_bound_scoid;
    }
    if (!may_bind || ConnectClientInfo(scoid, &ci, 0) == -1) {
        return 0;
    }
    if (ci.cred.euid != g_guardian_uid) {
        return 0;
    }
    g_bound_scoid = scoid;
    /* Published, not printed: no I/O on the message path (rt-c-review). */
    atomic_store_explicit(&g_telem->bound_pid, (int64_t)ci.pid, memory_order_relaxed);
    return 1;
}

/* -------------------------------------------------------------- messages */

static void reply(rcvid_t rcvid, uint32_t seq, uint16_t status, const arm_sample_t *s)
{
    arb_arm_reply_t r;

    memset(&r, 0, sizeof r);
    r.version = (uint16_t)ARB_ARM_PROTO_VERSION;
    r.status = status;
    r.seq = seq;
    r.pos_urad[0] = s->pos_urad[0];
    r.pos_urad[1] = s->pos_urad[1];
    r.vel_urad_s[0] = s->vel_urad_s[0];
    r.vel_urad_s[1] = s->vel_urad_s[1];
    r.arm_state = s->state;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreply.html */
    (void)MsgReply(rcvid, EOK, &r, sizeof r);
}

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

/* The arm's own checks of an approved setpoint, independent of the guardian. */
static void handle_setpoint(rcvid_t rcvid, const arb_arm_setpoint_t *sp, int64_t now)
{
    const int64_t tol = (int64_t)g_contract.vmax_urad_s * ARB_START_TOL_MS / 1000;
    arm_sample_t actual;
    uint32_t dur;

    arm_motion_sample(&g_motion, now, &actual);
    if (g_wd_tripped) {                                    /* braked by the watchdog: stay braked */
        count(&g_telem->rejected);
        reply(rcvid, sp->hdr.seq, ARB_ARM_REJECTED, &actual);
        return;
    }
    if (sp->hdr.version != ARB_ARM_PROTO_VERSION || sp->reserved != 0u ||
        sp->duration_ms < ARB_DURATION_MIN_MS || sp->duration_ms > g_contract.max_duration_ms) {
        count(&g_telem->rejected);
        reply(rcvid, sp->hdr.seq, ARB_ARM_REJECTED, &actual);
        return;
    }
    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        if (sp->target_urad[j] < g_contract.joint_min_urad[j] || sp->target_urad[j] > g_contract.joint_max_urad[j]) {
            count(&g_telem->rejected);
            reply(rcvid, sp->hdr.seq, ARB_ARM_REJECTED, &actual);
            return;
        }
        if (abs64((int64_t)sp->start_urad[j] - (int64_t)actual.pos_urad[j]) > tol) {
            reply(rcvid, sp->hdr.seq, ARB_ARM_STALE_START, &actual);
            return;
        }
    }
    /* FR-A3: never faster than vmax, whatever duration was asked for. */
    dur = arm_motion_min_duration_ms(actual.pos_urad, sp->target_urad, g_contract.vmax_urad_s);
    if (dur < sp->duration_ms) {
        dur = sp->duration_ms;
    }
    if (dur > g_contract.max_duration_ms) {
        count(&g_telem->rejected);
        reply(rcvid, sp->hdr.seq, ARB_ARM_REJECTED, &actual);
        return;
    }
    arm_motion_start(&g_motion, actual.pos_urad, sp->target_urad, dur, now);
    g_segment++;
    g_lease_valid = 1;
    g_lease_until_ns = now + LEASE_NS;
    reply(rcvid, sp->hdr.seq, ARB_ARM_OK, &actual);
}

static void handle_pulse(const struct _pulse *p, int64_t now)
{
    if (p->code == ARB_PULSE_ARM_WD_BRAKE) {
        if (arb_wd_client_is_trip(&g_wd, p, ARB_PULSE_ARM_WD_BRAKE)) {
            brake_latched(now);          /* FR-W2: the watchdog says the guardian is gone */
            g_wd_tripped = 1;            /* and no later setpoint may undo it (M1-J1) */
            count(&g_telem->wd_brakes);
        } else {
            count(&g_telem->rejected);   /* the code without our token: forged */
        }
        return;
    }
    switch (p->code) {
    case ARB_PULSE_ARM_TICK:
        if (!arb_own_pulse(&g_tick_scoid, p->scoid)) {
            count(&g_telem->rejected);   /* a tick not from our own timer (M1-J6) */
            return;
        }
        control_cycle(now);            /* motion is time-based: an extra tick cannot speed it up */
        return;
    case _PULSE_CODE_DISCONNECT:
        if (p->scoid == g_bound_scoid) {
            /* The guardian's connections are gone (closed or the process died):
             * brake now rather than wait for the lease. */
            brake_latched(now);
            g_bound_scoid = -1;
            atomic_store_explicit(&g_telem->bound_pid, (int64_t)-1, memory_order_relaxed);
            count(&g_telem->disconnects);
        }
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectdetach.html */
        (void)ConnectDetach(p->scoid);
        return;
    default:
        break;
    }
    if (p->scoid != g_bound_scoid || g_bound_scoid == -1) {
        count(&g_telem->rejected);      /* pulses from anyone but the guardian are ignored */
        return;
    }
    switch (p->code) {
    case ARB_PULSE_ARM_RENEW:
        if (g_lease_valid) {
            g_lease_until_ns = now + LEASE_NS;   /* an expired lease is not revived */
        }
        break;
    case ARB_PULSE_ARM_HOLD:
        arm_motion_stop(&g_motion, now, 0);
        break;
    case ARB_PULSE_ARM_BRAKE:
        brake_latched(now);
        break;
    default:
        count(&g_telem->rejected);
        break;
    }
}

/* ------------------------------------------------------------------ startup */

static int load_contract(const char *path)
{
    static uint8_t buf[sizeof(arb_contract_table_t) + 1u];
    ssize_t n;
    int fd = open(path, O_RDONLY);
    arb_load_result_t r;

    if (fd == -1) {
        fprintf(stderr, "arb-arm: cannot open contract %s: %s\n", path, strerror(errno));
        return -1;
    }
    n = read(fd, buf, sizeof buf);
    (void)close(fd);
    if (n < 0) {
        fprintf(stderr, "arb-arm: cannot read contract %s\n", path);
        return -1;
    }
    r = arb_contract_load(buf, (size_t)n, &g_contract);
    if (r != ARB_LOAD_OK) {
        fprintf(stderr, "arb-arm: contract %s refused (load code %d); not starting\n", path, (int)r);
        return -1;
    }
    return 0;
}

static int telemetry_create(void)
{
    int fd;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_unlink.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/ftruncate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
    (void)shm_unlink(ARB_TELEM_NAME);   /* a stale ring from an earlier run */
    fd = shm_open(ARB_TELEM_NAME, O_RDWR | O_CREAT | O_EXCL, 0644);   /* readers get read access only */
    if (fd == -1) {
        return -1;
    }
    if (ftruncate(fd, (off_t)sizeof(arb_telem_t)) == -1) {
        (void)close(fd);
        return -1;
    }
    g_telem = mmap(NULL, sizeof(arb_telem_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (g_telem == MAP_FAILED) {
        g_telem = NULL;
        return -1;
    }
    memset(g_telem, 0, sizeof *g_telem);
    g_telem->magic = ARB_TELEM_MAGIC;
    g_telem->version = ARB_TELEM_VERSION;
    g_telem->cycles_per_sec = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    atomic_store_explicit(&g_telem->bound_pid, (int64_t)-1, memory_order_relaxed);
    return 0;
}

int main(int argc, char **argv)
{
    static const int32_t home[ARB_JOINTS] = { 0, 0 };
    union {
        struct _pulse      pulse;
        uint16_t           type;
        arb_arm_hdr_t      hdr;
        arb_arm_setpoint_t sp;     /* the largest message; longer ones fail the srcmsglen check */
    } msg;
    struct _msg_info info;
    struct sched_param sp;
    struct sigevent ev;
    struct itimerspec its;
    name_attach_t *attach;
    dispatch_t *dpp;
    timer_t timer;
    int chid, coid, rc;
    int recv_errors = 0;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: arb-arm <contract.bin> [guardian-uid]\n");
        return 5;
    }
    if (load_contract(argv[1]) != 0) {
        return 2;
    }
    g_guardian_uid = argc == 3 ? (uid_t)strtoul(argv[2], NULL, 10) : 0;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedparam.html */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = ARB_PRIO_ARM;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        fprintf(stderr, "arb-arm: cannot set FIFO priority %d: %s\n", ARB_PRIO_ARM, strerror(rc));
        return 3;
    }
    /* NFR-I2: "In this QNX OS release, all memory is locked for the entire duration
     * of the process, so this function does nothing"; called to state the intent.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mlockall.html */
    (void)mlockall(MCL_CURRENT | MCL_FUTURE);

    if (telemetry_create() != 0) {
        fprintf(stderr, "arb-arm: cannot create telemetry %s: %s\n", ARB_TELEM_NAME, strerror(errno));
        return 5;
    }
    arm_backend_sim_bind(&g_backend);
    if (g_backend.init(&g_backend, home) != 0) {
        return 5;
    }
    arm_motion_init(&g_motion, home, now_ns());

    /* Own channel: no priority inheritance, disconnect pulses, and no
     * _NTO_CHF_UNBLOCK, so a guardian that times out is released by the kernel
     * (addendum section 3). Registered with name_attach() on our dispatch handle,
     * which keeps our channel (fr_g6_name_attach_uses_own_channel).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create_channel.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html */
    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT);
    if (chid == -1) {
        fprintf(stderr, "arb-arm: ChannelCreate: %s\n", strerror(errno));
        return 5;
    }
    dpp = dispatch_create_channel(chid, 0);
    if (dpp == NULL) {
        fprintf(stderr, "arb-arm: dispatch_create_channel: %s\n", strerror(errno));
        return 5;
    }
    attach = name_attach(dpp, ARB_ARM_NAME, 0);
    if (attach == NULL || attach->chid != chid) {
        fprintf(stderr, "arb-arm: name_attach %s failed or did not keep our channel\n", ARB_ARM_NAME);
        return 5;
    }

    /* 1 ms control-loop timer delivering a pulse at the arm's priority (FR-A1).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_settime.html */
    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        return 5;
    }
    if (arb_wd_client_register(&g_wd, ARB_WD_ROLE_ARM, coid, ARB_PRIO_ARM, ARB_PULSE_ARM_WD_BRAKE) != 0) {
        fprintf(stderr, "arb-arm: cannot register with the watchdog: %s; not starting\n", strerror(errno));
        return 6;
    }
    SIGEV_PULSE_INIT(&ev, coid, ARB_PRIO_ARM, ARB_PULSE_ARM_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        fprintf(stderr, "arb-arm: timer_create: %s\n", strerror(errno));
        return 5;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = ARB_ARM_PERIOD_NS;
    its.it_interval.tv_nsec = ARB_ARM_PERIOD_NS;
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        fprintf(stderr, "arb-arm: timer_settime: %s\n", strerror(errno));
        return 5;
    }
    {
        char hex[ARB_SHA256_HEX_LEN];

        arb_sha256_hex(g_contract.source_sha256, hex);
        printf("arb-arm: ready as %s, priority %d, backend %s, telemetry %s, contract sha256 %s\n", ARB_ARM_NAME,
               ARB_PRIO_ARM, g_backend.name, ARB_TELEM_NAME, hex);
    }
    fflush(stdout);

    /* The server loop: one message or pulse per iteration, for the life of the process.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html */
    for (;;) {
        const rcvid_t rcvid = MsgReceive(chid, &msg, sizeof msg, &info);
        const int64_t now = now_ns();
        arm_sample_t s;

        if (rcvid == -1) {
            /* M1-J18: do not spin at priority 70 on a receive that keeps failing.
             * Exiting drops the guardian's connection and the watchdog's
             * registration, and the frozen simulated arm moves no further. */
            if (++recv_errors >= ARB_RECV_ERRORS_MAX) {
                fprintf(stderr, "arb-arm: %d consecutive MsgReceive errors (%s); exiting\n", recv_errors,
                        strerror(errno));
                return 7;
            }
            continue;
        }
        recv_errors = 0;
        if (rcvid == 0) {
            handle_pulse(&msg.pulse, now);
            continue;
        }
        /* M1-J16: no field is read before the length says it is there. */
        if (info.srcmsglen < (ssize64_t)sizeof msg.type) {
            count(&g_telem->rejected);
            (void)MsgError(rcvid, EBADMSG);
            continue;
        }
        if (msg.type == _IO_CONNECT) {
            (void)MsgReply(rcvid, EOK, NULL, 0);   /* defensive: not observed from name_open() */
            continue;
        }
        /* M1-J16: a connection may become the bound guardian only through a
         * well-formed state query, the guardian's first message. */
        if (!authorized(info.scoid, msg.type == ARB_ARM_MSG_GET_STATE &&
                                    info.srcmsglen == (ssize64_t)sizeof(arb_arm_get_state_t))) {
            count(&g_telem->rejected);
            /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html */
            (void)MsgError(rcvid, EPERM);
            continue;
        }
        if (msg.type == ARB_ARM_MSG_GET_STATE && info.srcmsglen == (ssize64_t)sizeof(arb_arm_get_state_t)) {
            arm_motion_sample(&g_motion, now, &s);
            reply(rcvid, msg.hdr.seq, ARB_ARM_OK, &s);
        } else if (msg.type == ARB_ARM_MSG_SETPOINT && info.srcmsglen == (ssize64_t)sizeof(arb_arm_setpoint_t)) {
            handle_setpoint(rcvid, &msg.sp, now);
        } else {
            count(&g_telem->rejected);
            (void)MsgError(rcvid, msg.type <= _IO_MAX ? ENOSYS : EBADMSG);
        }
    }
}
