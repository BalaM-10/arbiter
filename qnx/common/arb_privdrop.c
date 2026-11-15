/*
 * NFR-I5 privilege drop; see arb_privdrop.h. Follows the 8.0 privilege
 * separation example: keep a short list of abilities for the non-root domain,
 * deny and lock the rest, then switch user.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_Privilege_separation_eg.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_Ability_dropping.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/abilities_reference.html
 */
#include "arb_privdrop.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/procmgr.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *step, int e, const char **what, int *err)
{
    *what = step;
    *err = e;
    return -1;
}

/* A denied call must fail with EPERM; success or another error means the drop did not take. */
static int denied(int rc, int e)
{
    return rc == -1 && e == EPERM;
}

int arb_privdrop(uid_t id, int prio_max, int keep_connection, uid_t query_lo, uid_t query_hi, const char **what,
                 int *err, char report[ARB_PRIVDROP_REPORT_LEN])
{
    const gid_t gid = (gid_t)id;
    struct sched_param sp;
    int policy, cur_prio, rc;
    pid_t child;

    report[0] = '\0';
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/g/geteuid.html */
    if (geteuid() != 0) {
        return fail("must start as root (it drops to its own user)", EPERM, what, err);
    }
    if (id == 0 || prio_max < 1 || (query_lo != 0 && query_hi < query_lo)) {
        return fail("bad arguments", EINVAL, what, err);
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_getschedparam.html */
    rc = pthread_getschedparam(pthread_self(), &policy, &sp);
    if (rc != EOK) {
        return fail("read own priority", rc, what, err);
    }
    cur_prio = sp.sched_priority;
    if (cur_prio > prio_max) {
        return fail("own priority above the kept range", EINVAL, what, err);
    }

    /* Non-root domain, in three calls so that no entry carries bounds it does not
     * use. Each kept ability is locked as it is set, so the final EOL entry, which
     * denies and locks "any unlocked abilities that you didn't specify", leaves it.
     * ALLOW and SUBRANGE must be ORed into ONE entry, followed by the two bounds
     * (inclusive): measured on QEMU, an ALLOW entry followed by a separate SUBRANGE
     * entry left every priority allowed (evidence/runs/2026-11-15-qemu/
     * privdrop-subrange-probe.log). The self-check below would catch that.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/procmgr_ability.html */
    rc = procmgr_ability(0,
                         PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_SUBRANGE | PROCMGR_AOP_LOCK |
                             PROCMGR_AID_PRIORITY,
                         (uint64_t)1, (uint64_t)prio_max,
                         PROCMGR_ADN_NONROOT | PROCMGR_AOP_LOCK | PROCMGR_AID_CONNECTION |
                             (keep_connection ? PROCMGR_AOP_ALLOW : PROCMGR_AOP_DENY),
                         PROCMGR_AID_EOL);
    if (rc != EOK) {
        return fail("procmgr_ability (priority, connection)", rc, what, err);
    }
    /* XPROCESS_QUERY's subrange is user IDs. Measured on QEMU: with 811-812, a uid
     * 811 process's /proc/<pid>/ctl can be read and a uid 820 one cannot
     * (evidence/runs/2026-11-15-qemu/procfs-query-probe.log). */
    if (query_lo != 0) {
        rc = procmgr_ability(0,
                             PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_SUBRANGE | PROCMGR_AOP_LOCK |
                                 PROCMGR_AID_XPROCESS_QUERY,
                             (uint64_t)query_lo, (uint64_t)query_hi, PROCMGR_AID_EOL);
        if (rc != EOK) {
            return fail("procmgr_ability (process query)", rc, what, err);
        }
    }
    rc = procmgr_ability(0, PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL);
    if (rc != EOK) {
        return fail("procmgr_ability (deny and lock the rest)", rc, what, err);
    }

    /* Groups first, then the user: once the user is not root, the group can no
     * longer be changed. Both set real, effective and saved IDs.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/setgroups.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/setregid.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/setreuid.html */
    if (setgroups(1, &gid) == -1) {
        return fail("setgroups", errno, what, err);
    }
    if (setregid(gid, gid) == -1) {
        return fail("setregid", errno, what, err);
    }
    if (setreuid(id, id) == -1) {
        return fail("setreuid", errno, what, err);
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/g/getuid.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/g/getegid.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/g/getgid.html */
    if (geteuid() != id || getuid() != id || getegid() != gid || getgid() != gid) {
        return fail("IDs not switched", EPERM, what, err);
    }

    /* Checks. Kept: the current priority can still be set. */
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedprio.html */
    rc = pthread_setschedprio(pthread_self(), cur_prio);
    if (rc != EOK) {
        return fail("check: own priority no longer allowed", rc, what, err);
    }
    /* Dropped: back to root. QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/setuid.html */
    rc = setuid(0);
    if (!denied(rc, errno)) {
        return fail("check: setuid(0) was not refused", rc == 0 ? 0 : errno, what, err);
    }
    /* Dropped: a priority above the kept range. */
    rc = pthread_setschedprio(pthread_self(), prio_max + 1);
    if (rc != EPERM) {
        if (rc == EOK) {
            (void)pthread_setschedprio(pthread_self(), cur_prio);
        }
        return fail("check: a priority above the kept range was not refused", rc, what, err);
    }
    /* Dropped: fork. If it ever succeeds, the child leaves at once and the caller exits.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fork.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/e/_exit.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/w/waitpid.html */
    child = fork();
    if (child == 0) {
        _exit(99);
    }
    if (child > 0) {
        (void)waitpid(child, NULL, 0);
        return fail("check: fork() was not refused", 0, what, err);
    }
    if (errno != EPERM) {
        return fail("check: fork() failed, but not with EPERM", errno, what, err);
    }

    {
        char query[48] = "";

        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/snprintf.html */
        if (query_lo != 0) {
            (void)snprintf(query, sizeof query, ", XPROCESS_QUERY %u-%u", (unsigned)query_lo, (unsigned)query_hi);
        }
        (void)snprintf(report, ARB_PRIVDROP_REPORT_LEN,
                       "privileges dropped: uid %u gid %u; kept PRIORITY 1-%d%s%s; refused as expected: setuid(0), "
                       "priority %d, fork()",
                       (unsigned)id, (unsigned)gid, prio_max, keep_connection ? ", CONNECTION" : "", query,
                       prio_max + 1);
    }
    return 0;
}
