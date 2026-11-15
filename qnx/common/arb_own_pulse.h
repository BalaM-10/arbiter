/*
 * Accept a pulse only if it came through a connection that the receiving
 * process itself holds (its timer's side-channel connection, on which the
 * watchdog's registered events are also delivered). Judge-review M1-J6: without
 * this, any process with a connection could send tick-coded pulses and make a
 * trusted thread run its tick work at that thread's priority.
 *
 * The first time, the kernel is asked who owns the sending connection; if it is
 * this process, that server connection ID is remembered and every later pulse
 * is compared with it, with no kernel call. A pulse from any other process is
 * rejected and the caller counts it.
 *
 * Not stated in the 8.0 page: that ConnectClientInfo() describes a connection a
 * process made to its own channel. Confirmed on target by the tests that need
 * genuine ticks to be accepted (fr_a6_telemetry_published_every_cycle,
 * fr_g7_guardian_sends_a_heartbeat_every_tick) and the forged-pulse tests.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectclientinfo.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/g/getpid.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/_pulse.html
 */
#ifndef ARB_OWN_PULSE_H
#define ARB_OWN_PULSE_H

#include <unistd.h>
#include <sys/neutrino.h>

/* *own_scoid starts at -1. Returns 1 if the pulse's scoid is our own connection. */
static inline int arb_own_pulse(int *own_scoid, int scoid)
{
    struct _client_info ci;

    if (*own_scoid != -1) {
        return scoid == *own_scoid;
    }
    if (ConnectClientInfo(scoid, &ci, 0) == -1 || ci.pid != getpid()) {
        return 0;
    }
    *own_scoid = scoid;
    return 1;
}

#endif /* ARB_OWN_PULSE_H */
