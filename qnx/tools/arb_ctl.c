/*
 * arb-ctl: the local operator console (FR-W4, FR-G5). Untrusted: it reaches the
 * guardian only through /dev/arbiter, like any other client.
 *
 *   arb-ctl status    print the guardian's state and counters
 *   arb-ctl estop     emergency stop: SAFE_STOP (accepted from any client)
 *   arb-ctl reset     operator reset of SAFE_STOP (accepted only from the operator's
 *                     user ID, given to arb-guardian at startup; decision D2)
 *
 * Exit status: 0 done, 1 refused or failed (the reason is printed), 2 usage.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "arb_contract.h"
#include "arb_devctl.h"

static const char *state_name(unsigned s)
{
    switch (s) {
    case ARB_STATE_HOLD:      return "HOLD";
    case ARB_STATE_NORMAL:    return "NORMAL";
    case ARB_STATE_SAFE_STOP: return "SAFE_STOP";
    default:                  return "UNKNOWN";
    }
}

static const char *cause_name(unsigned c)
{
    switch (c) {
    case ARB_STOP_NONE:        return "none";
    case ARB_STOP_VIOLATIONS:  return "violations";
    case ARB_STOP_ARM_TIMEOUT: return "arm_timeout";
    case ARB_STOP_WATCHDOG:    return "watchdog";
    case ARB_STOP_ESTOP:       return "estop";
    default:                   return "unknown";
    }
}

static int status(int fd)
{
    arb_status_t st;
    int rc;

    memset(&st, 0, sizeof st);
    rc = devctl(fd, ARB_DCMD_GET_STATUS, &st, sizeof st, NULL);
    if (rc != EOK) {
        printf("arb-ctl: status: %s\n", strerror(rc));
        return 1;
    }
    printf("state %s cause %s estops %u resets %u resets_refused %u watchdog_lost %u decisions %u "
           "submits_refused %u last_seq %u has_seq %u\n",
           state_name(st.state), cause_name(st.stop_cause), st.estops, st.resets, st.resets_refused,
           st.watchdog_lost, st.decisions, st.submits_refused, st.last_seq, st.has_seq);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc == 2 ? argv[1] : "";
    const int want_write = strcmp(cmd, "reset") == 0;
    int fd, rc;

    if (strcmp(cmd, "status") != 0 && strcmp(cmd, "estop") != 0 && !want_write) {
        fprintf(stderr, "usage: arb-ctl status|estop|reset\n");
        return 2;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html */
    fd = open(ARB_DEV_PATH, want_write ? O_RDWR : O_RDONLY);
    if (fd == -1) {
        printf("arb-ctl: %s: %s\n", ARB_DEV_PATH, strerror(errno));
        return 1;
    }
    if (strcmp(cmd, "status") == 0) {
        rc = status(fd);
    } else {
        rc = devctl(fd, want_write ? ARB_DCMD_RESET : ARB_DCMD_ESTOP, NULL, 0, NULL);
        printf("arb-ctl: %s: %s\n", cmd, rc == EOK ? "done" : strerror(rc));
        rc = rc == EOK ? 0 : 1;
        (void)status(fd);
    }
    (void)close(fd);
    return rc;
}
