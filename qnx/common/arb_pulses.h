/*
 * Every Arbiter pulse code, in one place.
 *
 * User pulse codes must lie in _PULSE_CODE_MINAVAIL .. _PULSE_CODE_MAXAVAIL;
 * negative codes are reserved for the kernel and managers.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/_pulse.html
 * In the 8.0 SDP header <sys/neutrino.h>: MINAVAIL = 0, MAXAVAIL = 127.
 */
#ifndef ARB_PULSES_H
#define ARB_PULSES_H

#include <sys/neutrino.h>

/* M0 message-passing sample (qnx/samples/msgpass). Not used by the guardian. */
#define ARB_PULSE_DEMO          (_PULSE_CODE_MINAVAIL + 0)
/* Guardian's own 1 ms timer tick (silence timeout; later heartbeats and lease renewals). */
#define ARB_PULSE_GUARDIAN_TICK (_PULSE_CODE_MINAVAIL + 1)

_Static_assert(ARB_PULSE_DEMO >= _PULSE_CODE_MINAVAIL && ARB_PULSE_DEMO <= _PULSE_CODE_MAXAVAIL,
               "pulse code outside the user range");
_Static_assert(ARB_PULSE_GUARDIAN_TICK >= _PULSE_CODE_MINAVAIL && ARB_PULSE_GUARDIAN_TICK <= _PULSE_CODE_MAXAVAIL,
               "pulse code outside the user range");

#endif /* ARB_PULSES_H */
