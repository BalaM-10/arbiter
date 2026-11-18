/*
 * arb-hog: the F12 CPU hog. An untrusted process, started only by the fault bench
 * (arb-inject's `hog` action) on the local QEMU target, as user ARB_UID_UNTRUSTED
 * (and type arb_untrusted_t on the security-policy image). It busy-loops, at the
 * highest priority an unprivileged process may use (ARB_PRIO_UNPRIVILEGED_MAX,
 * 63, FIFO), one thread per CPU, for a fixed time, then exits. It makes no
 * system call inside the loop except reading the clock about every 10 ms, and no
 * network use.
 * F12 shows that the trusted processes (64 and above) still run: the bench judges
 * that from the arm's telemetry and the watchdog's counters, never from here.
 *   arb-hog <seconds> <threads>      seconds 1..ARB_HOG_MAX_S, threads 1..ARB_HOG_MAX_THREADS
 * Output, after the loop ends:
 *   HOG start uid <n> euid <n> threads <n> seconds <n> pid <n>
 *   HOG thread <i> prio <n> policy <n> loops <n> ran_ms <n>
 *   HOG end ok <0|1>     (1: every thread ran at 63 FIFO for its time)
 * Exit: 0 ran, 2 usage, 3 started as root (an F12 result would mean nothing),
 *       4 a thread could not be created at its priority.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/o/on.html
 *   ("priorities of 64 and greater are privileged")
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_attr_setinheritsched.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_attr_setschedpolicy.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_attr_setschedparam.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_create.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_getschedparam.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arb_ids.h"
#include "arb_prio.h"

#define ARB_HOG_MAX_S        30      /* hard cap (owner decision 2026-11-17) */
#define ARB_HOG_MAX_THREADS  16

typedef struct {
    int64_t  until_ns;
    uint64_t loops;
    int64_t  ran_ns;
    int      prio, policy;
} hog_t;

static int64_t now_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *spin(void *arg)
{
    hog_t *h = arg;
    struct sched_param sp;
    const int64_t t0 = now_ns();
    int64_t t = t0;
    volatile uint64_t loops = 0;         /* volatile: the compiler keeps every iteration */

    (void)pthread_getschedparam(pthread_self(), &h->policy, &sp);
    h->prio = sp.sched_priority;
    /* Bounded by time: ends at until_ns, at most ARB_HOG_MAX_S after the start. */
    while (t < h->until_ns) {
        /* bound: 10 million, about 10 ms, then the clock: few kernel calls, so the
         * hog adds little to the kernel trace that F12 records */
        for (int i = 0; i < 10000000; i++) {
            loops = loops + 1u;
        }
        t = now_ns();
    }
    h->loops = loops;
    h->ran_ns = t - t0;
    return NULL;
}

int main(int argc, char **argv)
{
    static hog_t hogs[ARB_HOG_MAX_THREADS];
    static pthread_t tids[ARB_HOG_MAX_THREADS];
    pthread_attr_t attr;
    struct sched_param sp;
    long secs, n;
    int ok = 1, made = 0;
    int64_t until;

    if (argc != 3) {
        fprintf(stderr, "usage: arb-hog <seconds 1..%d> <threads 1..%d>\n", ARB_HOG_MAX_S, ARB_HOG_MAX_THREADS);
        return 2;
    }
    secs = strtol(argv[1], NULL, 10);
    n = strtol(argv[2], NULL, 10);
    if (secs < 1 || secs > ARB_HOG_MAX_S || n < 1 || n > ARB_HOG_MAX_THREADS) {
        fprintf(stderr, "arb-hog: seconds must be 1..%d, threads 1..%d\n", ARB_HOG_MAX_S, ARB_HOG_MAX_THREADS);
        return 2;
    }
    if (geteuid() == 0 || getuid() == 0) {
        fprintf(stderr, "arb-hog: refuses to run as root (F12 is about an untrusted process)\n");
        return 3;
    }
    until = now_ns() + (int64_t)secs * 1000000000LL;
    if (pthread_attr_init(&attr) != EOK ||
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != EOK ||
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != EOK) {
        fprintf(stderr, "arb-hog: thread attributes refused\n");
        return 4;
    }
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = ARB_PRIO_UNPRIVILEGED_MAX;
    if (pthread_attr_setschedparam(&attr, &sp) != EOK) {
        fprintf(stderr, "arb-hog: priority %d refused in the attributes\n", ARB_PRIO_UNPRIVILEGED_MAX);
        return 4;
    }
    for (long i = 0; i < n; i++) {                            /* bound: ARB_HOG_MAX_THREADS */
        int rc;
        hogs[i].until_ns = until;
        rc = pthread_create(&tids[i], &attr, spin, &hogs[i]);
        if (rc != EOK) {
            fprintf(stderr, "arb-hog: thread %ld at priority %d: %s\n", i, ARB_PRIO_UNPRIVILEGED_MAX, strerror(rc));
            ok = 0;
            break;
        }
        made++;
    }
    for (int i = 0; i < made; i++) {                          /* bound: ARB_HOG_MAX_THREADS */
        (void)pthread_join(tids[i], NULL);
    }
    printf("HOG start uid %d euid %d threads %d seconds %ld pid %d\n", (int)getuid(), (int)geteuid(), made, secs,
           (int)getpid());
    for (int i = 0; i < made; i++) {                          /* bound: ARB_HOG_MAX_THREADS */
        printf("HOG thread %d prio %d policy %d loops %llu ran_ms %lld\n", i, hogs[i].prio, hogs[i].policy,
               (unsigned long long)hogs[i].loops, (long long)(hogs[i].ran_ns / 1000000));
        ok = ok && hogs[i].prio == ARB_PRIO_UNPRIVILEGED_MAX && hogs[i].policy == SCHED_FIFO && hogs[i].loops > 0;
    }
    ok = ok && made == n;
    printf("HOG end ok %d\n", ok);
    (void)fflush(stdout);
    return made == n ? 0 : 4;
}
