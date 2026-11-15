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
    pid_t    server_pid;    /* who serves ARB_WD_NAME, and as which user (-1: unknown); F11 case 4 */
    int      server_uid;
    int      prio, code, beat_code;
} arb_wd_client_t;

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

/* Is this pulse our genuine trip event (right code and token)? */
int  arb_wd_client_is_trip(const arb_wd_client_t *w, const struct _pulse *p, int code);

/* FR-W6. A genuine beat (code and token) restarts the count and returns 1; a
 * pulse with the beat code but not our token returns 0 (forged; the caller counts it). */
int  arb_wd_client_beat(arb_wd_client_t *w, const struct _pulse *p, int beat_code);

/* FR-W6. Call on each of the caller's own 1 ms ticks: 1 once the watchdog has
 * been silent for ARB_WD_BEAT_TIMEOUT_MS ticks, else 0. */
int  arb_wd_client_tick_lost(arb_wd_client_t *w);

/* FR-W5. For a _PULSE_CODE_COIDDEATH pulse carrying `coid`: 1 if it is our
 * connection to the watchdog and its server is really gone, else 0. */
int  arb_wd_client_server_gone(const arb_wd_client_t *w, int coid);

#endif /* ARB_WD_CLIENT_H */
