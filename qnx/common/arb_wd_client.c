/* Watchdog client (see arb_wd_client.h and arb_watchdog_proto.h). */
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "arb_prio.h"
#include "arb_pulses.h"
#include "arb_watchdog_proto.h"
#include "arb_wd_client.h"

#define WD_REGISTER_TIMEOUT_NS 2000000ull   /* same bound as the guardian's arm sends */

/* Not a cryptographic secret: it only has to be unknown to other processes, so
 * that a pulse forged with the trip code is recognized and ignored. The
 * registered event itself can only be delivered by the watchdog. */
static uint64_t make_token(void)
{
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockcycles.html */
    uint64_t x = ClockCycles() ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)&x;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x | 1u;                      /* never zero */
}

int arb_wd_client_register(arb_wd_client_t *w, uint32_t role, int self_coid, int prio, int code, int beat_code)
{
    const uint64_t timeout_ns = WD_REGISTER_TIMEOUT_NS;
    arb_wd_register_t msg;
    arb_wd_reply_t reply;
    long rc;

    memset(w, 0, sizeof *w);
    w->role = role;
    w->self_coid = self_coid;
    w->prio = prio;
    w->code = code;
    w->beat_code = beat_code;
    w->token = make_token();
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html */
    w->coid = name_open(ARB_WD_NAME, 0);
    if (w->coid == -1) {
        return -1;
    }
    memset(&msg, 0, sizeof msg);
    msg.type = (uint16_t)ARB_WD_MSG_REGISTER;
    msg.version = (uint16_t)ARB_WD_PROTO_VERSION;
    msg.role = role;
    /* The trip event: a pulse to our own channel carrying the token.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigevent.html */
    if (prio < 1 || prio > 255 || code < _PULSE_CODE_MINAVAIL || code > _PULSE_CODE_MAXAVAIL ||
        beat_code < _PULSE_CODE_MINAVAIL || beat_code > _PULSE_CODE_MAXAVAIL || beat_code == code) {
        errno = EINVAL;                  /* the event's priority field is a short; keep it in range */
        return -1;
    }
    SIGEV_PULSE_INIT(&msg.event, self_coid, (short)prio, (short)code, 0);   /* both fields are short: range-checked above */
    msg.event.sigev_value.sival_long = (long)w->token;
    /* Only the watchdog may deliver it.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgregisterevent.html */
    if (MsgRegisterEvent(&msg.event, w->coid) == -1) {
        return -1;
    }
    /* FR-W6: the beat, a second registered event on the same connection (the
     * MsgRegisterEvent page's example registers two on one coid). */
    SIGEV_PULSE_INIT(&msg.beat, self_coid, (short)prio, (short)beat_code, 0);
    msg.beat.sigev_value.sival_long = (long)w->token;
    if (MsgRegisterEvent(&msg.beat, w->coid) == -1) {
        return -1;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html */
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns, NULL) == -1) {
        return -1;
    }
    rc = MsgSend(w->coid, &msg, sizeof msg, &reply, sizeof reply);
    if (rc == -1) {
        return -1;
    }
    if (reply.version != ARB_WD_PROTO_VERSION || reply.status != 0u) {
        errno = reply.status != 0u ? (int)reply.status : EBADMSG;
        return -1;
    }
    return 0;
}

int arb_wd_client_reregister(arb_wd_client_t *w)
{
    arb_wd_client_t n;

    if (arb_wd_client_register(&n, w->role, w->self_coid, w->prio, w->code, w->beat_code) != 0) {
        const int e = errno;
        if (n.coid != -1) {
            /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectdetach.html */
            (void)ConnectDetach(n.coid);
        }
        errno = e;
        return -1;
    }
    (void)ConnectDetach(w->coid);       /* the old connection, after the new one holds the role */
    *w = n;
    return 0;
}

void arb_wd_client_heartbeat(arb_wd_client_t *w)
{
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html */
    if (MsgSendPulse(w->coid, ARB_PRIO_WATCHDOG, ARB_PULSE_WD_HEARTBEAT, (long)w->role) == -1) {
        w->hb_errors++;
    }
}

int arb_wd_client_is_trip(const arb_wd_client_t *w, const struct _pulse *p, int code)
{
    return p->code == code && (uint64_t)p->value.sival_long == w->token;
}

int arb_wd_client_beat(arb_wd_client_t *w, const struct _pulse *p, int beat_code)
{
    if (p->code != beat_code || (uint64_t)p->value.sival_long != w->token) {
        return 0;
    }
    w->ticks_since_beat = 0;
    return 1;
}

int arb_wd_client_tick_lost(arb_wd_client_t *w)
{
    if (w->ticks_since_beat < (uint32_t)ARB_WD_BEAT_TIMEOUT_MS) {
        w->ticks_since_beat++;
        return 0;
    }
    return 1;                            /* stays 1 until a genuine beat */
}

int arb_wd_client_server_gone(const arb_wd_client_t *w, int coid)
{
    /* The ChannelCreate page's check: with info NULL, ConnectServerInfo() skips
     * connections with dead servers, so it returns another coid (or -1) when ours
     * is really gone; a pulse for a live connection is stale.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectserverinfo.html */
    return coid == w->coid && ConnectServerInfo(0, coid, NULL) != coid;
}
