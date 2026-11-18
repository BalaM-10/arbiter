/*
 * Watchdog client, used by the arm and the guardian (arb_watchdog_proto.h).
 * Trusted code: no allocation; the one MsgSend() is bounded by TimerTimeout().
 */
#ifndef ARB_WD_CLIENT_H
#define ARB_WD_CLIENT_H

#include <stdint.h>
#include <sys/neutrino.h>

typedef struct {
    int      coid;          /* connection to the watchdog */
    uint32_t role;
    uint64_t token;         /* value carried by our registered trip event */
    uint32_t hb_errors;     /* heartbeats the kernel refused */
    uint32_t ticks_since_beat;  /* own 1 ms ticks handled since the watchdog's last beat (FR-W6) */
    int      self_coid;     /* kept for arb_wd_client_reregister() */
    int      own_scoid;     /* server side of self_coid, learned on the first own pulse (-1: not yet; arb_own_pulse.h) */
    pid_t    server_pid;    /* who serves ARB_WD_NAME, and as which user (-1: unknown); F11 case 4 */
    int      server_uid;
    int      prio, code, beat_code;
    /* FR-G5 with FR-W6: after an operator reset, the token of the previous
     * registration, accepted for ARB_WD_STALE_TICKS of our own ticks (see below). */
    uint64_t prev_token;
    uint32_t prev_ticks_left;
    uint32_t stale;         /* events of the previous registration recognised (logged) */
} arb_wd_client_t;

/* After a re-registration, how many of the caller's own 1 ms ticks a pulse with
 * the previous token is still taken as the watchdog's own (10 beat periods). Such
 * a pulse can only come from the watchdog: a registered event may be delivered only
 * by "the only server permitted to send the event" (MsgRegisterEvent), and it
 * arrives on our own connection. The watchdog replaces a registration in one step
 * and then delivers only the new events, so a stale one was delivered before, and
 * waited in our queue ("the kernel queues it for delivery to the receiving
 * channel", MsgSendPulse) while we were registering. The 8.0 pages state no order
 * for pulses of equal priority, so the window is a time bound, not "until the
 * first new one".
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgregisterevent.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html */
#define ARB_WD_STALE_TICKS 20u

/* Connect and register. self_coid is a connection to the caller's own channel,
 * where the watchdog's trip event arrives as a pulse with code `code`, and its
 * beat (FR-W6) with code `beat_code`, both at priority `prio`. Returns 0, or -1
 * with errno set (no watchdog, refused, timeout). */
int  arb_wd_client_register(arb_wd_client_t *w, uint32_t role, int self_coid, int prio, int code, int beat_code);

/* FR-G5 operator reset: register again, with the same role and events, on a new
 * connection (to a watchdog that came back, or one that tripped us). The old
 * connection is closed only after the new registration holds the role, so the
 * watchdog never sees our role disconnect. On failure the old state is kept.
 * Bounded like arb_wd_client_register(). Returns 0, or -1 with errno set. */
int  arb_wd_client_reregister(arb_wd_client_t *w);

/* One heartbeat pulse (non-blocking). */
void arb_wd_client_heartbeat(arb_wd_client_t *w);

/* Is this pulse our genuine trip event? Right code, right token, and delivered
 * on our own connection self_coid, where the kernel puts the watchdog's
 * registered events (midpoint review M2): a pulse sent on any other connection
 * is refused whatever value it carries. A trip of the previous registration,
 * inside the stale window, is honoured too (fail-safe) and counted as stale. */
int  arb_wd_client_is_trip(arb_wd_client_t *w, const struct _pulse *p, int code);

/* FR-W6. A genuine beat (code, token and own connection, as for a trip) restarts
 * the count and returns 1. A beat of the previous registration, inside the stale
 * window, returns 1 too (not forged) but restarts nothing, and is counted as
 * stale. Any other pulse with the beat code returns 0 (forged; the caller counts it). */
int  arb_wd_client_beat(arb_wd_client_t *w, const struct _pulse *p, int beat_code);

/* FR-W6. Call on each of the caller's own 1 ms ticks: 1 once the watchdog has
 * been silent for ARB_WD_BEAT_TIMEOUT_MS ticks, else 0. */
int  arb_wd_client_tick_lost(arb_wd_client_t *w);

/* FR-W5. For a _PULSE_CODE_COIDDEATH pulse carrying `coid`: 1 if it is our
 * connection to the watchdog and its server is really gone, else 0. */
int  arb_wd_client_server_gone(const arb_wd_client_t *w, int coid);

#endif /* ARB_WD_CLIENT_H */
