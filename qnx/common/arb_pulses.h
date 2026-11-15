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
/* Guardian's own 1 ms timer tick (silence timeout, lease renewals). */
#define ARB_PULSE_GUARDIAN_TICK (_PULSE_CODE_MINAVAIL + 1)
/* Arm controller's own 1 ms control-loop tick. Motion depends on the arm's clock,
 * not on tick count, so a spoofed tick cannot speed the arm up. */
#define ARB_PULSE_ARM_TICK      (_PULSE_CODE_MINAVAIL + 2)
/* Guardian -> arm, accepted only from the bound guardian connection (FR-G6): */
#define ARB_PULSE_ARM_RENEW     (_PULSE_CODE_MINAVAIL + 3)   /* renew the motion lease */
#define ARB_PULSE_ARM_HOLD      (_PULSE_CODE_MINAVAIL + 4)   /* stop along the segment, not latched (HOLD) */
#define ARB_PULSE_ARM_BRAKE     (_PULSE_CODE_MINAVAIL + 5)   /* stop along the segment and latch (SAFE_STOP) */
/* Watchdog (FR-W1..W3): */
#define ARB_PULSE_WD_TICK          (_PULSE_CODE_MINAVAIL + 6)   /* watchdog's own 1 ms timer */
#define ARB_PULSE_WD_HEARTBEAT     (_PULSE_CODE_MINAVAIL + 7)   /* client -> watchdog, value = role */
#define ARB_PULSE_WD_DEATH         (_PULSE_CODE_MINAVAIL + 8)   /* process manager -> watchdog, value = pid */
#define ARB_PULSE_ARM_WD_BRAKE     (_PULSE_CODE_MINAVAIL + 9)   /* watchdog -> arm (registered event), value = token */
#define ARB_PULSE_GUARDIAN_WD_TRIP (_PULSE_CODE_MINAVAIL + 10)  /* watchdog -> guardian (registered event), value = token */
/* The watchdog's own heartbeat to each client (FR-W6), a registered event
 * delivered every ARB_WD_BEAT_PERIOD_MS, value = the client's token: */
#define ARB_PULSE_ARM_WD_BEAT      (_PULSE_CODE_MINAVAIL + 11)  /* watchdog -> arm */
#define ARB_PULSE_GUARDIAN_WD_BEAT (_PULSE_CODE_MINAVAIL + 12)  /* watchdog -> guardian */

_Static_assert(ARB_PULSE_DEMO >= _PULSE_CODE_MINAVAIL && ARB_PULSE_DEMO <= _PULSE_CODE_MAXAVAIL,
               "pulse code outside the user range");
_Static_assert(ARB_PULSE_GUARDIAN_WD_BEAT <= _PULSE_CODE_MAXAVAIL, "pulse code outside the user range");

#endif /* ARB_PULSES_H */
