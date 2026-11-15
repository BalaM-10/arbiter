/*
 * devctl() commands on /dev/arbiter (FR-G1). QNX only; kept out of arb_proto.h
 * so the pure library and its host tests do not depend on QNX headers.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/messages_HANDLING_devctl.html
 */
#ifndef ARB_DEVCTL_H
#define ARB_DEVCTL_H

#include <devctl.h>

#include "arb_proto.h"

#define ARB_DEV_PATH        "/dev/arbiter"
#define ARB_DCMD_SUBMIT     __DIOTF(_DCMD_MISC, 0x41, arb_submit_t)
#define ARB_DCMD_GET_STATUS __DIOF(_DCMD_MISC, 0x42, arb_status_t)
/* FR-W4: emergency stop, accepted from any client with /dev/arbiter open (stopping
 * is always allowed; decision D2). "__DION: Send a command with no associated data." */
#define ARB_DCMD_ESTOP      __DION(_DCMD_MISC, 0x43)
/* FR-G5: operator reset of SAFE_STOP. Only a descriptor opened for writing by the
 * operator's user ID (given to the guardian at startup); never through the gateway (D2).
 * Errors: EPERM not the operator, EBADF not opened for writing, EALREADY not in
 * SAFE_STOP, ENOLINK the watchdog is absent, EIO the arm refused or did not answer. */
#define ARB_DCMD_RESET      __DION(_DCMD_MISC, 0x44)

#endif /* ARB_DEVCTL_H */
