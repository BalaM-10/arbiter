/*
 * Test helper: make one devctl() on /dev/arbiter as a chosen effective user,
 * then return to root (FR-G10). The test programs run as root, because they
 * also kill and stop trusted processes and fork children as other users; only
 * the gateway's user (ARB_UID_GATEWAY) may submit commands, so each submit is
 * made with the effective user ID set to it for the duration of the call.
 *
 * The guardian asks the kernel who sent each message (ConnectClientInfo() on the
 * message's scoid), so the same descriptor submitting as root is refused: that
 * is what fr_g10_only_the_gateway_user_may_submit relies on.
 *
 * seteuid(): a process may set its effective user ID to its real or saved
 * set-user ID, or to any ID its PROCMGR_AID_SETUID ability allows; root's real
 * and saved IDs stay 0, so it can return to 0.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/seteuid.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html
 */
#ifndef ARB_AS_GATEWAY_H
#define ARB_AS_GATEWAY_H

#include <devctl.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>

#include "arb_ids.h"

#define ARB_AS_FAILED 255   /* could not switch user: never a devctl() result here */

/* devctl() on fd as effective user euid; returns devctl()'s errno-style result,
 * or ARB_AS_FAILED if the user could not be switched (or switched back). */
static int arb_devctl_as(uid_t euid, int fd, int dcmd, void *data, size_t n)
{
    int rc;

    if (seteuid(euid) == -1) {
        return ARB_AS_FAILED;
    }
    rc = devctl(fd, dcmd, data, n, NULL);
    if (seteuid(0) == -1) {
        return ARB_AS_FAILED;
    }
    return rc;
}

/* The usual case: as the gateway. */
static int arb_devctl_gw(int fd, int dcmd, void *data, size_t n)
{
    return arb_devctl_as((uid_t)ARB_UID_GATEWAY, fd, dcmd, data, n);
}

#endif /* ARB_AS_GATEWAY_H */
