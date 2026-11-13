/*
 * Every Arbiter thread priority, in one place (docs/DESIGN_M1_ADDENDUM.md section 4).
 * Order, owner decision 2026-11-13: arm loop > watchdog > guardian > logger > untrusted.
 *
 * The three trusted processes run in the privileged range (64 and above), which
 * a process without PROCMGR_AID_PRIORITY cannot use, so no untrusted process can
 * run at, or send a message or pulse at, their priority (TM-C3):
 *   "priorities of 64 and greater are privileged, so only processes with the
 *    PROCMGR_AID_PRIORITY ability enabled ... can use them"
 *   QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/o/on.html
 * Priorities 254 and 255 are the clock and IPI interrupt service threads:
 *   QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/p/procnto.html
 */
#ifndef ARB_PRIO_H
#define ARB_PRIO_H

#define ARB_PRIO_ARM        70
#define ARB_PRIO_WATCHDOG   66
#define ARB_PRIO_GUARDIAN   64
#define ARB_PRIO_LOGGER     30
#define ARB_PRIO_UNTRUSTED  20   /* gateway and test clients */

#define ARB_PRIO_UNPRIVILEGED_MAX 63

_Static_assert(ARB_PRIO_ARM > ARB_PRIO_WATCHDOG && ARB_PRIO_WATCHDOG > ARB_PRIO_GUARDIAN &&
               ARB_PRIO_GUARDIAN > ARB_PRIO_LOGGER && ARB_PRIO_LOGGER > ARB_PRIO_UNTRUSTED,
               "priority order");
_Static_assert(ARB_PRIO_GUARDIAN > ARB_PRIO_UNPRIVILEGED_MAX, "trusted threads must be privileged");
_Static_assert(ARB_PRIO_ARM < 254, "254 and 255 are reserved for interrupt service threads");

#endif /* ARB_PRIO_H */
