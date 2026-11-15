/* F11 case 4: see arb_server_check.h. */
#include "arb_server_check.h"

#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>

/* The server's pid, from the kernel. ConnectServerInfo() returns the first
 * connection >= coid, so the result must be coid itself; a dead server's
 * connection (_NTO_COF_DEAD) is not a server.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectserverinfo.html */
static pid_t server_pid_of(int coid)
{
    struct _server_info si;

    if (ConnectServerInfo(0, coid, &si) != coid || (si.flags & _NTO_COF_DEAD) != 0) {
        return -1;
    }
    return si.pid;
}

int arb_server_check(int coid, uid_t expected, pid_t *server_pid, int *server_uid)
{
    char path[32];
    procfs_info info;
    pid_t pid;
    int fd, rc;

    *server_pid = -1;
    *server_uid = -1;
    pid = server_pid_of(coid);
    if (pid <= 0) {
        errno = ENOTCONN;
        return -1;
    }
    *server_pid = pid;

    /* "You must open the appropriate file (e.g., /proc/pid/ctl), not the
     * /proc/pid directory." Another user's process can be opened only with
     * PROCMGR_AID_XPROCESS_QUERY for its user IDs; otherwise open() fails (EPERM,
     * measured), and the server is refused.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_proc_filesystem.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html */
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/snprintf.html */
    (void)snprintf(path, sizeof path, "/proc/%d/ctl", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd == -1) {
        if (errno == EACCES) {
            errno = EPERM;
        }
        return -1;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_DCMD_PROC_INFO.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html */
    memset(&info, 0, sizeof info);
    rc = devctl(fd, DCMD_PROC_INFO, &info, sizeof info, NULL);
    (void)close(fd);
    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    if (info.pid != pid) {
        errno = ESRCH;
        return -1;
    }
    *server_uid = (int)info.euid;
    if (info.uid != expected || info.euid != expected) {
        errno = EPERM;
        return -1;
    }
    /* The same process must still own the channel: it did not die (and its pid
     * get reused) while we looked. */
    if (server_pid_of(coid) != pid) {
        errno = ESRCH;
        return -1;
    }
    return 0;
}
