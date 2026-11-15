/*
 * arb-guardian: the trusted decision point between the untrusted side and the arm.
 *
 * A single-threaded QNX resource manager at /dev/arbiter (FR-G1). Each command
 * arrives as a devctl() and is decided with libcontract (FR-G3, FR-G4); only an
 * approved setpoint is forwarded to the arm, through a send bounded with
 * TimerTimeout() (FR-G9). A 1 ms timer pulse drives the silence timeout (FR-G5).
 * Layout follows the 8.0 single-threaded resource manager skeleton:
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/skeleton_SIMPLE_ST_EG.html
 *
 *   arb-guardian <contract.bin>
 *
 * Exit status: 2 contract refused (FR-C4), 3 scheduling, 4 no arm, 5 setup, 6 no watchdog.
 * Startup order: watchdog, then arm, then guardian. The guardian registers with
 * the watchdog, sends a heartbeat every tick, and enters SAFE_STOP when the
 * watchdog says the arm died or is stuck.
 * Design: docs/DESIGN_M1_ADDENDUM.md. Threat model: docs/THREAT_MODEL.md.
 * Toward the arm (docs/DESIGN_M1_ADDENDUM.md section 2): a lease renewal pulse
 * every tick unless in SAFE_STOP; a hold pulse on entering HOLD; a latching brake
 * pulse on entering SAFE_STOP. Logs go to slogger2 (arb_log.h, NFR-Q6): startup,
 * state changes and faults, written by the server loop after each message is
 * handled and replied to, never from the decision path. Not yet (M2): operator reset.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>      /* before sys/dispatch.h, as the 8.0 skeleton requires */
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/stat.h>

#include "arb_contract.h"
#include "arb_devctl.h"
#include "arb_hex.h"
#include "arb_log.h"
#include "arb_own_pulse.h"
#include "arb_prio.h"
#include "arb_pulses.h"
#include "arm_link.h"
#include "arb_watchdog_proto.h"
#include "arb_wd_client.h"

#define TICK_NS 1000000L
#define ARB_RECV_ERRORS_MAX 100   /* consecutive receive failures before exiting (M1-J18) */

/* resmgr_attach() keeps pointers to these: they must be static.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/resmgr_attach.html */
static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t      g_io_funcs;
static iofunc_attr_t          g_attr;

/* Decision state. One owner: the single resource-manager thread. */
static arb_contract_table_t g_contract;
static arb_guard_t          g_guard;
static arm_link_t           g_arm;
static arb_status_t         g_status;
static int                  g_tick_scoid = -1;      /* our own timer's connection (arb_own_pulse.h) */
static arb_wd_client_t      g_wd;

/* What the system log last said (NFR-Q6). Owner: the same thread, after each message. */
static struct {
    uint8_t  state, cause;
    uint32_t arm_faults, watchdog_trips, foreign_pulses, bad_messages, arm_pulse_errors;
} g_logged;

static uint64_t now_ns(void)
{
    struct timespec ts;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;   /* CLOCK_MONOTONIC cannot fail with a valid pointer; 0 makes any silence look long */
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------- pulses to the arm */

/* Non-blocking notification to the arm, at the arm's priority. A failure is
 * counted; the arm's lease stops it anyway if renewals stop arriving.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html */
static void arm_pulse(int code)
{
    if (MsgSendPulse(g_arm.coid, ARB_PRIO_ARM, code, 0) == -1) {
        g_status.arm_pulse_errors++;
    }
}

/* Tell the arm about a state change: HOLD stops along the segment, SAFE_STOP brakes and latches. */
static void follow_state(uint8_t before)
{
    if (g_guard.state == before) {
        return;
    }
    if (g_guard.state == ARB_STATE_SAFE_STOP) {
        arm_pulse(ARB_PULSE_ARM_BRAKE);
    } else if (g_guard.state == ARB_STATE_HOLD) {
        arm_pulse(ARB_PULSE_ARM_HOLD);
    }
}

/* ------------------------------------------------------------------ decide */

static void arm_fault(arb_verdict_t *v)
{
    g_status.arm_faults++;
    arb_guard_fault(&g_guard, ARB_STOP_ARM_TIMEOUT);
    arb_guard_reject_after_approval(&g_guard, ARB_RULE_ARM_FAULT, v);
}

/* One command: checks 1-5 without the arm; ask the arm where it is only if they
 * pass; checks 6-7 from there; forward only an approved setpoint. */
static void decide(const void *payload, size_t len, arb_verdict_t *v)
{
    const uint64_t now = now_ns();
    int32_t pos[ARB_JOINTS];
    arb_cmd_t cmd;

    g_status.decisions++;
    if (!arb_guard_check_pre(&g_guard, payload, len, now, &cmd, v)) {
        return;
    }
    if (arm_link_get_state(&g_arm, pos) != ARM_LINK_OK) {
        arm_fault(v);
        return;
    }
    arb_guard_check_motion(&g_guard, &cmd, pos, now, v);
    if (v->verdict == ARB_VERDICT_DENY) {
        return;
    }
    switch (arm_link_send_setpoint(&g_arm, pos, v->target_urad, v->duration_ms)) {
    case ARM_LINK_OK:
        arb_guard_commit_accept(&g_guard, now, v);
        break;
    case ARM_LINK_STALE:
        arb_guard_reject_after_approval(&g_guard, ARB_RULE_STALE_START, v);
        break;
    case ARM_LINK_TIMEOUT:
    case ARM_LINK_ERROR:
    default:
        arm_fault(v);
        break;
    }
}

static void to_wire(const arb_verdict_t *v, arb_verdict_wire_t *w)
{
    memset(w, 0, sizeof *w);
    w->version = (uint16_t)ARB_PROTO_VERSION;
    w->verdict = v->verdict;
    w->rule = v->rule;
    w->state = v->state_after;
    w->flags = v->flags;
    w->seq = v->seq;
    w->target_urad[0] = v->target_urad[0];
    w->target_urad[1] = v->target_urad[1];
    w->duration_ms = v->duration_ms;
    w->requested_ms = v->requested_ms;
}

/* ----------------------------------------------------- resource manager I/O */

/*
 * devctl() handler. Message checking follows the 8.0 System Security Guide:
 * trust ctp->size and ctp->info.srcmsglen, never the client's nbytes or
 * combine_len; check the client's reply buffer (ctp->info.dstmsglen) and return
 * an error rather than let data be cut; check the open mode.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/write_resource_manager.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/messages_HANDLING_devctl.html
 * Errors are returned as the errno value itself (8.0: no _RESMGR_ERRNO).
 */
static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb)
{
    const size_t hdr = sizeof msg->i;
    int status;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_devctl_default.html */
    status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) {
        return status;
    }
    if (ctp->rcvid == 0) {                 /* part of a combine message: no reply possible */
        g_status.bad_messages++;
        return EBADMSG;
    }

    switch (msg->i.dcmd) {
    case ARB_DCMD_SUBMIT: {
        const size_t want = hdr + sizeof(arb_submit_t);
        arb_cmd_t cmd_copy;
        arb_verdict_t v;
        arb_verdict_wire_t w;

        if ((ocb->ioflag & _IO_FLAG_WR) == 0) {
            g_status.bad_messages++;
            return EBADF;                  /* commands need a descriptor opened for writing */
        }
        if (ctp->info.srcmsglen != (ssize64_t)want || ctp->size < want ||
            ctp->info.dstmsglen < (ssize64_t)want) {
            g_status.bad_messages++;
            return EBADMSG;                /* exact size in, room for the full verdict out */
        }
        /* Copy the command out first: the reply is written over the same buffer. */
        memcpy(&cmd_copy, _IO_INPUT_PAYLOAD(msg), sizeof cmd_copy);
        {
            const uint8_t before = g_guard.state;
            decide(&cmd_copy, sizeof cmd_copy, &v);
            follow_state(before);
        }
        to_wire(&v, &w);
        memset(&msg->o, 0, sizeof msg->o);
        memcpy(_IO_OUTPUT_PAYLOAD(msg), &w, sizeof w);
        return _RESMGR_PTR(ctp, &msg->o, want);
    }
    case ARB_DCMD_GET_STATUS: {
        const size_t want = hdr + sizeof(arb_status_t);

        if ((ocb->ioflag & _IO_FLAG_RD) == 0) {
            g_status.bad_messages++;
            return EBADF;
        }
        if (ctp->info.dstmsglen < (ssize64_t)want) {
            g_status.bad_messages++;
            return EBADMSG;
        }
        g_status.version = (uint16_t)ARB_PROTO_VERSION;
        g_status.state = g_guard.state;
        g_status.stop_cause = g_guard.stop_cause;
        g_status.clients = g_attr.count;
        memset(&msg->o, 0, sizeof msg->o);
        memcpy(_IO_OUTPUT_PAYLOAD(msg), &g_status, sizeof g_status);
        return _RESMGR_PTR(ctp, &msg->o, want);
    }
    default:
        g_status.bad_messages++;
        return ENOTTY;                     /* devctl(): "The device doesn't support the dcmd command." */
    }
}

/*
 * Last close of an open: runs when a client closes /dev/arbiter or terminates
 * for any reason, so a dead gateway is noticed at once (TM-C4).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_Client_termination.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_close_ocb_default.html
 */
static int io_close_ocb(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb)
{
    const int status = iofunc_close_ocb_default(ctp, reserved, ocb);
    const uint8_t before = g_guard.state;

    if (g_attr.count == 0u) {
        (void)arb_guard_client_lost(&g_guard);
        follow_state(before);
    }
    return status;
}

/* 1 ms tick: silence timeout (FR-G5) and the arm's lease renewal (FR-A4).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pulse_attach.html */
static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    const uint8_t before = g_guard.state;

    (void)code; (void)flags; (void)handle;
    /* The pulse is ctp->msg->pulse (struct _pulse, with scoid), per the 8.0
     * pulse_attach() page and <sys/resmgr.h>.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pulse_attach.html */
    if (!arb_own_pulse(&g_tick_scoid, ctp->msg->pulse.scoid)) {
        g_status.foreign_pulses++;        /* a tick not from our own timer (M1-J6) */
        return 0;
    }
    g_status.ticks++;
    (void)arb_guard_tick(&g_guard, now_ns());
    follow_state(before);
    if (g_guard.state != ARB_STATE_SAFE_STOP) {
        arm_pulse(ARB_PULSE_ARM_RENEW);   /* HOLD is an authorized state too */
    }
    arb_wd_client_heartbeat(&g_wd);
    return 0;
}

/* The watchdog says the arm died or is stuck: SAFE_STOP (FR-W2, FR-G5). */
static int on_wd_trip(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    const uint8_t before = g_guard.state;

    (void)code; (void)flags; (void)handle;
    if (!arb_wd_client_is_trip(&g_wd, &ctp->msg->pulse, ARB_PULSE_GUARDIAN_WD_TRIP)) {
        g_status.foreign_pulses++;        /* the code without our token: forged */
        return 0;
    }
    g_status.watchdog_trips++;
    arb_guard_fault(&g_guard, ARB_STOP_WATCHDOG);
    follow_state(before);
    return 0;
}

/* ------------------------------------------------------------- system log */

static const char *state_text(uint8_t state, uint8_t cause)
{
    switch (state) {
    case ARB_STATE_NORMAL:
        return "state NORMAL";
    case ARB_STATE_HOLD:
        return "state HOLD (no command within the silence timeout, or no client)";
    case ARB_STATE_SAFE_STOP:
        switch (cause) {
        case ARB_STOP_VIOLATIONS:  return "state SAFE_STOP, cause violations (threshold of counted denials)";
        case ARB_STOP_ARM_TIMEOUT: return "state SAFE_STOP, cause arm timeout (a bounded send to the arm failed)";
        case ARB_STOP_WATCHDOG:    return "state SAFE_STOP, cause watchdog (the arm died or is stuck)";
        case ARB_STOP_ESTOP:       return "state SAFE_STOP, cause e-stop";
        default:                   return "state SAFE_STOP, cause unknown";
        }
    default:
        return "state unknown";
    }
}

/* A counter moved: log it (the first 16 times, then once per 1024, arb_log.h). */
static void log_count(uint32_t *logged, uint32_t now, uint16_t code, uint8_t sev, const char *fmt)
{
    if (arb_log_worth(*logged, now)) {
        arb_log_1(code, sev, fmt, (int64_t)now);
    }
    *logged = now;
}

/* NFR-Q6: write what changed since the last call. Called by the server loop
 * after each message or pulse is handled (so after its reply), and once at
 * startup with first = 1. Bounded: a fixed number of comparisons, at most six
 * bounded slog2 writes. */
static void log_changes(int first)
{
    if (first || g_guard.state != g_logged.state || g_guard.stop_cause != g_logged.cause) {
        arb_log_c(ARB_LOG_STATE, g_guard.state == ARB_STATE_SAFE_STOP ? SLOG2_WARNING : SLOG2_INFO,
                  state_text(g_guard.state, g_guard.stop_cause));
        g_logged.state = g_guard.state;
        g_logged.cause = g_guard.stop_cause;
    }
    log_count(&g_logged.arm_faults, g_status.arm_faults, ARB_LOG_FAULT, SLOG2_ERROR,
              "a bounded send to the arm failed or timed out (%lld so far)");
    log_count(&g_logged.watchdog_trips, g_status.watchdog_trips, ARB_LOG_TRIP, SLOG2_WARNING,
              "watchdog trip received: the arm died or is stuck (%lld so far)");
    log_count(&g_logged.foreign_pulses, g_status.foreign_pulses, ARB_LOG_REFUSED, SLOG2_WARNING,
              "ignored a tick or trip pulse not from its own source (%lld so far)");
    log_count(&g_logged.bad_messages, g_status.bad_messages, ARB_LOG_REFUSED, SLOG2_NOTICE,
              "refused a malformed or unknown devctl (%lld so far)");
    log_count(&g_logged.arm_pulse_errors, g_status.arm_pulse_errors, ARB_LOG_FAULT, SLOG2_ERROR,
              "the kernel refused a pulse to the arm (%lld so far)");
}

/* ------------------------------------------------------------------ startup */

/* FR-C4: load the table once, refuse to start on any problem. */
static int load_contract(const char *path)
{
    static uint8_t buf[sizeof(arb_contract_table_t) + 1u];
    ssize_t n;
    int fd;
    arb_load_result_t r;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/read.html */
    fd = open(path, O_RDONLY);
    if (fd == -1) {
        arb_log_s(ARB_LOG_FAIL, SLOG2_ERROR, "cannot open contract %s", path);
        arb_log_fail("open contract", errno);
        return -1;
    }
    n = read(fd, buf, sizeof buf);        /* one byte more than a table, to detect oversize */
    (void)close(fd);
    if (n < 0) {
        arb_log_fail("read contract", errno);
        return -1;
    }
    r = arb_contract_load(buf, (size_t)n, &g_contract);
    if (r != ARB_LOAD_OK) {
        arb_log_fail_code("contract refused (FR-C4)", (int64_t)r);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sched_param sp;
    struct sigevent ev;
    struct itimerspec its;
    resmgr_attr_t rattr;
    dispatch_t *dpp;
    dispatch_context_t *ctp;
    int recv_errors = 0;
    timer_t timer;
    int chid, coid, rc;

    (void)arb_log_init("arb_guardian", "arb-guardian");   /* first: startup failures are logged too */
    if (argc != 2) {
        fprintf(stderr, "usage: arb-guardian <contract.bin>\n");
        return 5;
    }
    if (load_contract(argv[1]) != 0) {
        return 2;
    }

    /* Fixed FIFO priority in the privileged range (docs/DESIGN_M1_ADDENDUM.md section 4).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedparam.html */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = ARB_PRIO_GUARDIAN;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        arb_log_fail("set FIFO priority", rc);
        return 3;
    }

    if (arm_link_open(&g_arm) != 0) {
        arb_log_fail("find the arm controller " ARB_ARM_NAME, errno);
        return 4;
    }
    /* One bounded query before serving anyone: proves the arm answers, and makes
     * the arm bind this connection as the guardian's (FR-G6) before the first
     * lease renewal is sent. */
    {
        int32_t pos[ARB_JOINTS];
        if (arm_link_get_state(&g_arm, pos) != ARM_LINK_OK) {
            arb_log_fail("first state query to the arm", g_arm.last_errno);
            return 4;
        }
    }

    /* Our own channel, so its flags are ours: no priority inheritance from clients,
     * plus the unblock and disconnect pulses the dispatch page asks a resource
     * manager's channel to have.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create_channel.html */
    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT);
    if (chid == -1) {
        arb_log_fail("ChannelCreate", errno);
        return 5;
    }
    dpp = dispatch_create_channel(chid, DISPATCH_FLAG_NOLOCK);
    if (dpp == NULL) {
        arb_log_fail("dispatch_create_channel", errno);
        return 5;
    }

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_func_init.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_attr_init.html */
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs, _RESMGR_IO_NFUNCS, &g_io_funcs);
    g_io_funcs.devctl = io_devctl;
    g_io_funcs.close_ocb = io_close_ocb;
    /* M1: 0666 while every process runs as root; M2 narrows access with users and
     * the security policy (FR-S3). */
    iofunc_attr_init(&g_attr, S_IFNAM | 0666, NULL, NULL);

    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;
    if (resmgr_attach(dpp, &rattr, ARB_DEV_PATH, _FTYPE_ANY, 0, &g_connect_funcs, &g_io_funcs, &g_attr) == -1) {
        arb_log_fail("resmgr_attach " ARB_DEV_PATH, errno);
        return 5;
    }

    /* Everything attached before dispatch_context_alloc() (DISPATCH_FLAG_NOLOCK). */
    if (pulse_attach(dpp, 0, ARB_PULSE_GUARDIAN_TICK, on_tick, NULL) == -1 ||
        pulse_attach(dpp, 0, ARB_PULSE_GUARDIAN_WD_TRIP, on_wd_trip, NULL) == -1) {
        arb_log_fail("pulse_attach", errno);
        return 5;
    }

    /* 1 ms periodic timer delivering a pulse at the guardian's own priority.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigevent.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_settime.html */
    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        arb_log_fail("ConnectAttach", errno);
        return 5;
    }
    if (arb_wd_client_register(&g_wd, ARB_WD_ROLE_GUARDIAN, coid, ARB_PRIO_GUARDIAN, ARB_PULSE_GUARDIAN_WD_TRIP) != 0) {
        arb_log_fail("register with the watchdog", errno);
        return 6;
    }
    SIGEV_PULSE_INIT(&ev, coid, ARB_PRIO_GUARDIAN, ARB_PULSE_GUARDIAN_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        arb_log_fail("timer_create", errno);
        return 5;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = TICK_NS;
    its.it_interval.tv_nsec = TICK_NS;

    arb_guard_init(&g_guard, &g_contract, now_ns());
    if (timer_settime(timer, 0, &its, NULL) == -1) {
        arb_log_fail("timer_settime", errno);
        return 5;
    }

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_context_alloc.html */
    ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        arb_log_fail("dispatch_context_alloc", errno);
        return 5;
    }
    {
        char hex[ARB_SHA256_HEX_LEN];

        arb_sha256_hex(g_contract.source_sha256, hex);
        printf("arb-guardian: ready on %s, priority %d, contract sha256 %s\n", ARB_DEV_PATH, ARB_PRIO_GUARDIAN,
               hex);
        arb_log_s(ARB_LOG_START, SLOG2_INFO, "ready on " ARB_DEV_PATH ", contract sha256 %s", hex);
        log_changes(1);
    }
    fflush(stdout);

    /* The server loop: blocks in MsgReceive() inside dispatch_block() and handles
     * one message or pulse per iteration. It runs for the life of the process.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_block.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_handler.html */
    for (;;) {
        dispatch_context_t *got = dispatch_block(ctp);
        if (got == NULL) {
            /* e.g. EINTR; the old context stays valid. M1-J18: not forever at
             * priority 64; exiting drops the arm's connection, so the arm brakes. */
            if (++recv_errors >= ARB_RECV_ERRORS_MAX) {
                arb_log_fail("dispatch_block, 100 consecutive errors; exiting", errno);
                return 7;
            }
            continue;
        }
        recv_errors = 0;
        ctp = got;
        (void)dispatch_handler(ctp);   /* handles the message and sends its reply */
        log_changes(0);                /* only then: nothing logged before the reply */
    }
}
