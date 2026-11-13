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
} arb_wd_client_t;

/* Connect and register. self_coid is a connection to the caller's own channel,
 * where the watchdog's trip event will arrive as a pulse with code `code` at
 * priority `prio`. Returns 0, or -1 with errno set (no watchdog, refused, timeout). */
int  arb_wd_client_register(arb_wd_client_t *w, uint32_t role, int self_coid, int prio, int code);

/* One heartbeat pulse (non-blocking). */
void arb_wd_client_heartbeat(arb_wd_client_t *w);

/* Is this pulse our genuine trip event (right code and token)? */
int  arb_wd_client_is_trip(const arb_wd_client_t *w, const struct _pulse *p, int code);

#endif /* ARB_WD_CLIENT_H */
