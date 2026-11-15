/*
 * F11 case 4 (name squatting): a client checks who serves a name before it
 * trusts the connection.
 *
 * name_open() finds whoever registered the name first: "There's no guarantee
 * that the client will end up talking to the server it thinks it's connecting
 * to" [name_open]. Without a security policy, any process may register
 * arbiter_arm or arbiter_watchdog before the real one starts. So, before its
 * first message, a trusted client asks the kernel which process owns the
 * channel behind its connection (ConnectServerInfo()), reads that process's
 * user IDs from /proc/<pid>/ctl (DCMD_PROC_INFO), and accepts the server only if
 * its real and effective user IDs are both the expected one. The client keeps
 * PROCMGR_AID_XPROCESS_QUERY for exactly the users it checks (arb_privdrop.h), so
 * a server of any other user cannot even be queried, and fails the check.
 *
 * Returns 0 if the server is the expected user; -1 otherwise, with errno EPERM
 * (another user, or one we may not query) or the errno of the failed call.
 * *server_pid and *server_uid are set when known (else -1).
 */
#ifndef ARB_SERVER_CHECK_H
#define ARB_SERVER_CHECK_H

#include <sys/types.h>

int arb_server_check(int coid, uid_t expected, pid_t *server_pid, int *server_uid);

#endif /* ARB_SERVER_CHECK_H */
