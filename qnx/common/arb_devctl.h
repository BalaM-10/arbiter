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

#endif /* ARB_DEVCTL_H */
