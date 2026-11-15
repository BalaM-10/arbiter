/*
 * Watchdog protocol (FR-W1 to FR-W3; docs/DESIGN_M1_ADDENDUM.md section 2).
 *
 * The arm and the guardian are clients of the watchdog. Each one, at startup:
 *   1. name_open()s the watchdog's internal name;
 *   2. builds a pulse event aimed at its own channel, carrying a random token,
 *      registers it with MsgRegisterEvent(&event, watchdog_coid) so that only the
 *      watchdog may deliver it (8.0 refuses unregistered events with EACCES);
 *   3. sends ARB_WD_MSG_REGISTER with its role and that event (bounded send).
 * Then it sends ARB_PULSE_WD_HEARTBEAT (value = role) every cycle.
 *
 * The watchdog never sends a message to a client. When it trips, it delivers the
 * other client's registered event with MsgDeliverEvent() (non-blocking):
 *   guardian died or stuck -> the arm's event   (arm brakes and latches)
 *   arm died or stuck      -> the guardian's event (guardian enters SAFE_STOP)
 * The client checks the token, so a pulse forged with the same code is ignored.
 *
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgregisterevent.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgdeliverevent.html
 */
#ifndef ARB_WATCHDOG_PROTO_H
#define ARB_WATCHDOG_PROTO_H

#include <stdatomic.h>
#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/siginfo.h>

#define ARB_WD_NAME            "arbiter_watchdog"           /* /dev/name/local/arbiter_watchdog */
#define ARB_WD_PROTO_VERSION   1u
#define ARB_WD_TIMEOUT_MS      10                           /* missed heartbeats -> "stuck" */
/* FR-W6: the watchdog beats each client every 2 ms; a client that has seen no
 * beat for 10 ms treats the watchdog as lost (owner decision D7, 2026-11-14;
 * QEMU function first, Pi 5 values in M3). */
#define ARB_WD_BEAT_PERIOD_MS  2
#define ARB_WD_BEAT_TIMEOUT_MS 10

/* Why a client decided the watchdog is gone (FR-W5, FR-W6). */
enum {
    ARB_WD_LOST_NONE       = 0,
    ARB_WD_LOST_CONNECTION = 1,   /* _PULSE_CODE_COIDDEATH: its channel was destroyed (it died) */
    ARB_WD_LOST_BEATS      = 2    /* no beat for ARB_WD_BEAT_TIMEOUT_MS (stopped, stuck or dead) */
};

enum { ARB_WD_MSG_REGISTER = _IO_MAX + 16 };

enum {
    ARB_WD_ROLE_GUARDIAN = 1,
    ARB_WD_ROLE_ARM      = 2
};

enum {
    ARB_WD_CAUSE_NONE         = 0,
    ARB_WD_CAUSE_DIED         = 1,   /* process-manager death notice, confirmed */
    ARB_WD_CAUSE_STUCK        = 2,   /* alive, heartbeats missing */
    ARB_WD_CAUSE_DISCONNECTED = 3    /* closed its connection to the watchdog while alive */
};

typedef struct {
    uint16_t        type;            /* ARB_WD_MSG_REGISTER */
    uint16_t        version;         /* ARB_WD_PROTO_VERSION */
    uint32_t        role;            /* ARB_WD_ROLE_* */
    struct sigevent event;           /* registered with MsgRegisterEvent(&event, watchdog coid) */
} arb_wd_register_t;

typedef struct {
    uint16_t version;
    uint16_t status;                 /* 0 accepted; otherwise an errno value */
    uint32_t reserved;
} arb_wd_reply_t;

/* Read-only status record in shared memory, written only by the watchdog. */
#define ARB_WD_STATUS_NAME  "/arbiter_watchdog_status"
#define ARB_WD_STATUS_MAGIC 0x53474457u                    /* "WDGS" */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t cycles_per_sec;
    _Atomic int64_t  pid[3];             /* [role]: registered process, or -1 */
    _Atomic uint64_t heartbeats[3];      /* [role]: heartbeats accepted */
    _Atomic uint64_t last_hb_cycles[3];  /* [role]: ClockCycles() of the last accepted heartbeat */
    _Atomic uint64_t death_notices;      /* confirmed death notices for a monitored process (current or last) */
    _Atomic uint64_t death_pulses;       /* every death pulse received (diagnostic) */
    _Atomic int64_t  last_death_value;   /* pid carried by the last death pulse (diagnostic) */
    _Atomic int64_t  self_scoid;         /* scoid of the watchdog's own connection (authenticates kernel pulses) */
    _Atomic uint64_t trips;              /* trips recorded */
    _Atomic uint32_t last_role;          /* role of the last trip */
    _Atomic uint32_t last_cause;         /* ARB_WD_CAUSE_* of the last trip */
    _Atomic int64_t  last_pid;
    _Atomic uint64_t last_cycles;        /* ClockCycles() at the last trip */
    _Atomic uint64_t delivered;          /* events delivered to the other client */
    _Atomic uint64_t deliver_errors;
    _Atomic uint64_t rejected;           /* bad registrations, foreign heartbeats, unconfirmed deaths */
} arb_wd_status_t;

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "64-bit atomics must be lock-free for shared memory");

#endif /* ARB_WATCHDOG_PROTO_H */
