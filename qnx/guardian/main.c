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
 *   arb-guardian <contract.bin> [operator-uid]   (no operator uid: reset refused to everyone)
 *
 * Exit status: 2 contract refused (FR-C4), 3 scheduling, 4 no arm, 5 setup, 6 no watchdog,
 * 8 privilege drop failed.
 * Privileges (NFR-I5): started as root; once /dev/arbiter is attached it switches
 * to user ARB_UID_GUARDIAN for good, keeping only PRIORITY (up to its own 64) and
 * CONNECTION (pulses to the arm and the watchdog, other users), before it
 * registers with the watchdog or queries the arm, so both see the guardian's own
 * user (arb_privdrop.h).
 * Startup order: watchdog, then arm, then guardian. The guardian registers with
 * the watchdog, sends a heartbeat every tick, and enters SAFE_STOP when the
 * watchdog says the arm died or is stuck. It also enters SAFE_STOP (cause
 * WATCHDOG) on its own when the watchdog itself is lost (fail-safe): the
 * connection to it dies (_PULSE_CODE_COIDDEATH, FR-W5) or its beat stops for
 * ARB_WD_BEAT_TIMEOUT_MS ticks (FR-W6).
 * Commands (FR-G10): ARB_DCMD_SUBMIT only from the gateway's user, as the kernel
 * reports the sender, on one submitter open at a time; when that open closes,
 * NORMAL goes to HOLD at once. The status publishes the last sequence number.
 * E-stop (FR-W4): ARB_DCMD_ESTOP from any client enters SAFE_STOP (cause ESTOP)
 * and brakes the arm before the reply. Operator reset (FR-G5): ARB_DCMD_RESET,
 * only from the operator's user ID on a descriptor opened for writing, only in
 * SAFE_STOP; the arm and then the guardian register again with the watchdog,
 * and the guardian goes to HOLD. Never reachable through the gateway (D2).
 * Design: docs/DESIGN_M1_ADDENDUM.md. Threat model: docs/THREAT_MODEL.md.
 * Toward the arm (docs/DESIGN_M1_ADDENDUM.md section 2): a lease renewal pulse
 * every tick unless in SAFE_STOP; a hold pulse on entering HOLD; a latching brake
 * pulse on entering SAFE_STOP. Logs go to slogger2 (arb_log.h, NFR-Q6): startup,
 * state changes and faults, written by the server loop after each message is
 * handled and replied to, never from the decision path.
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syspage.h>

#include "arb_contract.h"
#include "arb_decisions.h"
#include "arb_devctl.h"
#include "arb_hex.h"
#include "arb_ids.h"
#include "arb_log.h"
#include "arb_own_pulse.h"
#include "arb_prio.h"
#include "arb_privdrop.h"
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
static arb_dec_ring_t      *g_dec;                  /* FR-G8: the decision ring, written only here */
static int                  g_tick_scoid = -1;      /* our own timer's connection (arb_own_pulse.h) */
static arb_wd_client_t      g_wd;
static int                  g_wd_lost;              /* the watchdog itself is gone (FR-W5, FR-W6) */
static int                  g_operator_set;         /* FR-G5: an operator user ID was given */
static uid_t                g_operator_uid;
/* FR-G10: the one open that commands come from (the gateway's), or NULL. Set on
 * its first accepted SUBMIT, cleared when that open closes. Compared only, never
 * dereferenced after the close (io_close_ocb clears it before the OCB is freed). */
static const RESMGR_OCB_T  *g_submitter;

/* What the system log last said (NFR-Q6). Owner: the same thread, after each message. */
static struct {
    uint8_t  state, cause;
    uint32_t arm_faults, watchdog_trips, foreign_pulses, bad_messages, arm_pulse_errors, watchdog_lost;
    uint32_t estops, resets, resets_refused, submits_refused, wd_stale;
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

/* FR-W4: stop now. Any client may (stopping is always allowed, decision D2). */
static void estop(void)
{
    const uint8_t before = g_guard.state;

    g_status.estops++;
    arb_guard_fault(&g_guard, ARB_STOP_ESTOP);
    follow_state(before);                 /* the brake pulse goes before the reply */
}

/* FR-G5 operator reset (the caller checked who is asking). Returns EOK or the
 * errno documented in arb_devctl.h. Bounded: one arm send (10 ms) and one
 * watchdog registration (2 ms), only reachable in SAFE_STOP. */
static int operator_reset(void)
{
    const uint8_t before = g_guard.state;

    if (g_guard.state != ARB_STATE_SAFE_STOP) {
        return EALREADY;
    }
    /* The arm first: it registers again with the watchdog and clears its latch,
     * or refuses when there is no watchdog (FR-W5). */
    switch (arm_link_reset(&g_arm)) {
    case ARM_LINK_OK:
        break;
    case ARM_LINK_NO_WATCHDOG:
        return ENOLINK;
    default:
        return EIO;
    }
    /* Then the guardian, last, so its heartbeats resume at once. */
    if (arb_wd_client_reregister(&g_wd) != 0) {
        return ENOLINK;
    }
    g_wd_lost = 0;
    if (arb_guard_reset(&g_guard, now_ns()) != 0) {
        return EALREADY;
    }
    g_status.resets++;
    follow_state(before);                 /* HOLD: lease renewals resume on the next tick */
    return EOK;
}

/* FR-G5: is this client the operator? Its effective user ID, from the kernel.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectclientinfo.html */
static int is_operator(int scoid)
{
    struct _client_info ci;

    if (!g_operator_set || ConnectClientInfo(scoid, &ci, 0) == -1) {
        return 0;
    }
    return ci.cred.euid == g_operator_uid;
}

/* FR-G10: commands only from the gateway's user, as the kernel reports the
 * sender of this message (not the opener of the descriptor, and not a field the
 * client wrote). Same check as is_operator().
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectclientinfo.html */
static int is_gateway(int scoid)
{
    struct _client_info ci;

    if (ConnectClientInfo(scoid, &ci, 0) == -1) {
        return 0;                          /* unsure: refuse */
    }
    return ci.cred.euid == (uid_t)ARB_UID_GATEWAY;
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
/* FR-G8, FR-E1, NFR-Q4: one record per decided command, before the reply.
 * Single writer, no lock and no blocking call: fill the slot, then publish the
 * new head (release), as arb_decisions.h describes. Bounded: one copy. */
static void record_decision(const arb_cmd_t *c, const arb_verdict_t *v, uint64_t t_ns, uint64_t t_rx, uint64_t t_done)
{
    const uint64_t h = atomic_load_explicit(&g_dec->head, memory_order_relaxed);
    arb_dec_record_t *r = &g_dec->r[h % ARB_DEC_SLOTS];

    r->index = h;
    r->t_ns = (int64_t)t_ns;
    r->t_cycles = t_rx;
    r->latency_cycles = t_done - t_rx;
    r->cmd = *c;
    r->verdict = *v;
    r->reserved = 0;
    atomic_store_explicit(&g_dec->head, h + 1u, memory_order_release);
}

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
        /* NFR-Q4: the cycle counter at receipt.
         * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockcycles.html */
        const uint64_t t_rx = ClockCycles();
        const size_t want = hdr + sizeof(arb_submit_t);
        arb_cmd_t cmd_copy;
        arb_verdict_t v;
        arb_verdict_wire_t w;

        if ((ocb->ioflag & _IO_FLAG_WR) == 0) {
            g_status.bad_messages++;
            return EBADF;                  /* commands need a descriptor opened for writing */
        }
        if (!is_gateway(ctp->info.scoid)) {
            g_status.submits_refused++;
            return EPERM;                  /* FR-G10: not the gateway's user */
        }
        if (g_submitter != NULL && g_submitter != ocb) {
            g_status.submits_refused++;
            return EBUSY;                  /* FR-G10: another open is the submitter */
        }
        if (ctp->info.srcmsglen != (ssize64_t)want || ctp->size < want ||
            ctp->info.dstmsglen < (ssize64_t)want) {
            g_status.bad_messages++;
            return EBADMSG;                /* exact size in, room for the full verdict out */
        }
        g_submitter = ocb;                 /* FR-G10: this open is the submitter until it closes */
        /* Copy the command out first: the reply is written over the same buffer. */
        memcpy(&cmd_copy, _IO_INPUT_PAYLOAD(msg), sizeof cmd_copy);
        {
            const uint8_t before = g_guard.state;
            const uint64_t t_ns = now_ns();
            decide(&cmd_copy, sizeof cmd_copy, &v);
            follow_state(before);
            record_decision(&cmd_copy, &v, t_ns, t_rx, ClockCycles());
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
        g_status.last_seq = g_guard.last_seq;      /* FR-G10: a restarted planner continues after it */
        g_status.has_seq = g_guard.have_seq;
        memset(&msg->o, 0, sizeof msg->o);
        memcpy(_IO_OUTPUT_PAYLOAD(msg), &g_status, sizeof g_status);
        return _RESMGR_PTR(ctp, &msg->o, want);
    }
    case ARB_DCMD_ESTOP:                   /* FR-W4: any client, any open mode */
        estop();
        memset(&msg->o, 0, sizeof msg->o);
        return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
    case ARB_DCMD_RESET: {                 /* FR-G5: the operator only */
        int rc;

        if ((ocb->ioflag & _IO_FLAG_WR) == 0) {
            rc = EBADF;
        } else if (!is_operator(ctp->info.scoid)) {
            rc = EPERM;
        } else {
            rc = operator_reset();
        }
        if (rc != EOK) {
            g_status.resets_refused++;
            return rc;
        }
        memset(&msg->o, 0, sizeof msg->o);
        return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
    }
    default:
        g_status.bad_messages++;
        return ENOTTY;                     /* devctl(): "The device doesn't support the dcmd command." */
    }
}

/*
 * Last close of an open: runs when a client closes /dev/arbiter or terminates
 * for any reason. When it is the submitter's open, NORMAL goes to HOLD at once,
 * whatever other clients (logger, dashboard relay, arb-ctl) still hold the device
 * open, so a dead gateway is noticed at once (TM-C4, FR-G5, FR-G10).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_Client_termination.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_close_ocb_default.html
 */
static int io_close_ocb(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb)
{
    const int was_submitter = ocb == g_submitter;   /* before the default frees the OCB */
    const uint8_t before = g_guard.state;
    int status;

    if (was_submitter) {
        g_submitter = NULL;
    }
    status = iofunc_close_ocb_default(ctp, reserved, ocb);
    if (was_submitter) {
        (void)arb_guard_client_lost(&g_guard);
        follow_state(before);
    }
    return status;
}

static void watchdog_lost(uint16_t cause);   /* FR-W5 / FR-W6, below */

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
    /* FR-W6: counted in our own ticks, so a late tick of ours cannot look like a
     * silent watchdog (its beat queued ahead of this tick is handled first). */
    if (arb_wd_client_tick_lost(&g_wd)) {
        watchdog_lost(ARB_WD_LOST_BEATS);
    }
    (void)arb_guard_tick(&g_guard, now_ns());
    follow_state(before);
    if (g_guard.state != ARB_STATE_SAFE_STOP) {
        arm_pulse(ARB_PULSE_ARM_RENEW);   /* HOLD is an authorized state too */
    }
    arb_wd_client_heartbeat(&g_wd);
    return 0;
}

/* FR-W5 / FR-W6: without its supervisor the guardian stops the system. SAFE_STOP
 * stays until an operator reset (M2 P4); a watchdog that comes back changes nothing. */
static void watchdog_lost(uint16_t cause)
{
    const uint8_t before = g_guard.state;

    if (g_wd_lost) {
        return;
    }
    g_wd_lost = 1;
    g_status.watchdog_lost++;
    g_status.watchdog_lost_cause = cause;
    arb_guard_fault(&g_guard, ARB_STOP_WATCHDOG);
    follow_state(before);                 /* brake pulse to the arm */
}

/* FR-W6: the watchdog's beat, a registered event carrying our token, on our own connection. */
static int on_wd_beat(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)code; (void)flags; (void)handle;
    if (!arb_wd_client_beat(&g_wd, &ctp->msg->pulse, ARB_PULSE_GUARDIAN_WD_BEAT)) {
        g_status.foreign_pulses++;        /* not our token, or not on our own connection: forged */
    }
    return 0;
}

/* FR-W5: a server we are connected to destroyed its channel; the value is the
 * coid of our connection [ChannelCreate]. Only the watchdog's counts here (the
 * arm's death is the watchdog's to report, FR-W2).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html */
static int on_coid_death(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)code; (void)flags; (void)handle;
    if (arb_wd_client_server_gone(&g_wd, ctp->msg->pulse.value.sival_int)) {
        watchdog_lost(ARB_WD_LOST_CONNECTION);
    }
    return 0;
}

/* The watchdog says the arm died or is stuck: SAFE_STOP (FR-W2, FR-G5). */
static int on_wd_trip(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    const uint8_t before = g_guard.state;

    (void)code; (void)flags; (void)handle;
    if (!arb_wd_client_is_trip(&g_wd, &ctp->msg->pulse, ARB_PULSE_GUARDIAN_WD_TRIP)) {
        g_status.foreign_pulses++;        /* not our token, or not on our own connection: forged */
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
 * startup with first = 1. Bounded: a fixed number of comparisons and bounded
 * slog2 writes (at most one per counter). */
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
    log_count(&g_logged.estops, g_status.estops, ARB_LOG_STATE, SLOG2_WARNING,
              "emergency stop received (%lld so far)");
    log_count(&g_logged.resets, g_status.resets, ARB_LOG_STATE, SLOG2_WARNING,
              "operator reset: SAFE_STOP left for HOLD (%lld so far)");
    log_count(&g_logged.resets_refused, g_status.resets_refused, ARB_LOG_REFUSED, SLOG2_WARNING,
              "operator reset refused: not the operator, not opened for writing, not stopped, "
              "no watchdog, or the arm refused (%lld so far)");
    log_count(&g_logged.wd_stale, g_wd.stale, ARB_LOG_CLIENT, SLOG2_INFO,
              "recognised a watchdog event of the previous registration, still queued at the operator reset: "
              "the watchdog's own, not forged (%lld so far)");
    log_count(&g_logged.submits_refused, g_status.submits_refused, ARB_LOG_REFUSED, SLOG2_WARNING,
              "refused a command: not the gateway's user, or not the submitter's open (%lld so far)");
    if (g_status.watchdog_lost != g_logged.watchdog_lost) {
        g_logged.watchdog_lost = g_status.watchdog_lost;
        arb_log_c(ARB_LOG_FAULT, SLOG2_ERROR,
                  g_status.watchdog_lost_cause == ARB_WD_LOST_CONNECTION
                      ? "watchdog lost (its connection died): SAFE_STOP"
                      : "watchdog lost (no beat for 10 ticks): SAFE_STOP");
    }
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

/* FR-G8: create the decision ring while still root: mode 0640 and the logger's
 * group, so the logger may read it and the gateway's user may not open it
 * (NFR-I1). Not stated in the 8.0 pages: that fchown() applies to a shared
 * memory object; confirmed on target by nfr_i1_gateway_user_cannot_open_the_decision_ring.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_unlink.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fchown.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/ftruncate.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/syspage_entry.html */
static int decisions_create(void)
{
    void *p;
    int fd;

    (void)shm_unlink(ARB_DEC_NAME);     /* a stale ring from an earlier run */
    fd = shm_open(ARB_DEC_NAME, O_RDWR | O_CREAT | O_EXCL, 0640);
    if (fd == -1) {
        return -1;
    }
    if (fchown(fd, (uid_t)-1, (gid_t)ARB_UID_LOGD) == -1 || ftruncate(fd, (off_t)sizeof(arb_dec_ring_t)) == -1) {
        const int e = errno;
        (void)close(fd);
        (void)shm_unlink(ARB_DEC_NAME);
        errno = e;
        return -1;
    }
    p = mmap(NULL, sizeof(arb_dec_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (p == MAP_FAILED) {
        return -1;
    }
    g_dec = p;
    memset(g_dec, 0, sizeof *g_dec);
    g_dec->magic = ARB_DEC_MAGIC;
    g_dec->version = ARB_DEC_VERSION;
    g_dec->cycles_per_sec = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
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
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: arb-guardian <contract.bin> [operator-uid]\n");
        return 5;
    }
    if (argc == 3) {
        g_operator_uid = (uid_t)strtoul(argv[2], NULL, 10);
        g_operator_set = 1;
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

    /* Our own channel, so its flags are ours: no priority inheritance from clients,
     * plus the unblock and disconnect pulses the dispatch page asks a resource
     * manager's channel to have.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create_channel.html */
    /* _NTO_CHF_COID_DISCONNECT (FR-W5): a pulse when the watchdog's channel is destroyed. */
    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT);
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
    /* 0666: any user may open; what each may do is checked per message (FR-G10:
     * commands only from the gateway's user; FR-G5: reset only from the operator;
     * FR-W4: e-stop from anyone). On the policy image, only the listed types may
     * connect at all (FR-S2). */
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
        pulse_attach(dpp, 0, ARB_PULSE_GUARDIAN_WD_TRIP, on_wd_trip, NULL) == -1 ||
        pulse_attach(dpp, 0, ARB_PULSE_GUARDIAN_WD_BEAT, on_wd_beat, NULL) == -1 ||
        /* A predefined _PULSE_CODE_* code is allowed; only UNBLOCK and DISCONNECT
         * are excluded with resmgr_attach() [pulse_attach]. */
        pulse_attach(dpp, 0, _PULSE_CODE_COIDDEATH, on_coid_death, NULL) == -1) {
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
    if (decisions_create() != 0) {
        arb_log_fail("create the decision ring " ARB_DEC_NAME, errno);
        return 5;
    }
    /* NFR-I5: root is needed no further (the path is attached); the watchdog and
     * the arm must see the guardian's own user. */
    {
        char report[ARB_PRIVDROP_REPORT_LEN];
        const char *what;
        int err;

        if (arb_privdrop(ARB_UID_GUARDIAN, ARB_PRIO_GUARDIAN, 1, ARB_UID_ARM, ARB_UID_WATCHDOG, &what, &err, report) != 0) {
            arb_log_fail(what, err);
            return 8;
        }
        printf("arb-guardian: %s\n", report);
        arb_log_s(ARB_LOG_START, SLOG2_INFO, "%s", report);
    }
    if (arm_link_open(&g_arm) != 0) {
        const int e = errno;
        if (e == EPERM && g_arm.server_pid != -1) {
            arb_log_impostor(ARB_ARM_NAME, g_arm.server_pid, g_arm.server_uid, ARB_UID_ARM);   /* F11 case 4 */
        }
        arb_log_fail("find the arm controller " ARB_ARM_NAME, e);
        return 4;
    }
    /* One bounded query before serving anyone: proves the arm answers, and makes
     * the arm bind this connection as the guardian's (FR-G6) before the first
     * lease renewal is sent. Clients that open /dev/arbiter meanwhile wait: the
     * channel is not received on until the server loop. */
    {
        int32_t pos[ARB_JOINTS];
        if (arm_link_get_state(&g_arm, pos) != ARM_LINK_OK) {
            arb_log_fail("first state query to the arm", g_arm.last_errno);
            return 4;
        }
    }
    SIGEV_PULSE_INIT(&ev, coid, ARB_PRIO_GUARDIAN, ARB_PULSE_GUARDIAN_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        arb_log_fail("timer_create", errno);
        return 5;
    }
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = TICK_NS;
    its.it_interval.tv_nsec = TICK_NS;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_context_alloc.html */
    ctp = dispatch_context_alloc(dpp);
    if (ctp == NULL) {
        arb_log_fail("dispatch_context_alloc", errno);
        return 5;
    }
    /* NFR-Q6, FR-W2: every write to stdout happens here, flushed, before the
     * watchdog supervises the guardian. stdout to a pipe is buffered, so without
     * the flush these lines would be written after the registration, before the
     * first heartbeat, and a write that blocks there trips the guardian as stuck
     * (run-182322). A blocked stdout now delays the start instead (not
     * registered, /dev/arbiter not served, the arm without a lease: nothing
     * moves). From the registration on: the system log only. */
    {
        char hex[ARB_SHA256_HEX_LEN];

        arb_sha256_hex(g_contract.source_sha256, hex);
        printf("arb-guardian: ready on %s, priority %d, contract sha256 %s\n", ARB_DEV_PATH, ARB_PRIO_GUARDIAN,
               hex);
        (void)fflush(stdout);    /* a lost line must not stop the start */
        if (arb_wd_client_register(&g_wd, ARB_WD_ROLE_GUARDIAN, coid, ARB_PRIO_GUARDIAN, ARB_PULSE_GUARDIAN_WD_TRIP,
                                   ARB_PULSE_GUARDIAN_WD_BEAT) != 0) {
            const int e = errno;
            if (e == EPERM && g_wd.server_pid != -1) {
                arb_log_impostor(ARB_WD_NAME, g_wd.server_pid, g_wd.server_uid, ARB_UID_WATCHDOG);   /* F11 case 4 */
            }
            arb_log_fail("register with the watchdog", e);
            return 6;
        }
        arb_guard_init(&g_guard, &g_contract, now_ns());
        if (timer_settime(timer, 0, &its, NULL) == -1) {
            arb_log_fail_supervised("timer_settime", errno);
            return 5;
        }
        arb_log_s(ARB_LOG_START, SLOG2_INFO, "ready on " ARB_DEV_PATH ", contract sha256 %s", hex);
        log_changes(1);
    }

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
                arb_log_fail_supervised("dispatch_block, 100 consecutive errors; exiting", errno);
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
