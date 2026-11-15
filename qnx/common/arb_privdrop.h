/*
 * NFR-I5: a trusted process drops root after its privileged setup.
 *
 * arb_privdrop() keeps, for the non-root user the process becomes:
 *   - PROCMGR_AID_PRIORITY, limited to priorities 1..prio_max (owner decision
 *     2026-11-13: every trusted process keeps it), and
 *   - PROCMGR_AID_CONNECTION if keep_connection is set (pulses to a process of
 *     another user: MsgSendPulse(), SIGEV_PULSE),
 *   - PROCMGR_AID_XPROCESS_QUERY limited to user IDs query_lo..query_hi, if
 *     query_lo is not 0: reading /proc/<pid>/ctl of a server of exactly those
 *     users, to check who serves a name before trusting it (arb_server_check.h),
 * denies and locks every other ability, sets its groups to {id}, and switches its
 * real, effective and saved group and user IDs to id. It then checks that the drop
 * took: setuid(0), fork() and a priority above prio_max must each fail with EPERM,
 * and setting the current priority must still succeed. Any other outcome is a
 * failure, and the caller exits: a trusted process does not run half-dropped.
 *
 * Call it from the main thread, as root, before any other thread exists.
 * Returns 0, or -1 with *what naming the failed step and *err its errno.
 * On success, *report holds one line describing what was kept and checked.
 */
#ifndef ARB_PRIVDROP_H
#define ARB_PRIVDROP_H

#include <sys/types.h>

#define ARB_PRIVDROP_REPORT_LEN 224

int arb_privdrop(uid_t id, int prio_max, int keep_connection, uid_t query_lo, uid_t query_hi, const char **what,
                 int *err, char report[ARB_PRIVDROP_REPORT_LEN]);

#endif /* ARB_PRIVDROP_H */
