/*
 * FR-G9 design check: does a 2 ms TimerTimeout() on MsgSend() work on this
 * target, and how long does it actually take to fire? Measures, does not assume
 * (docs/DESIGN_M1_ADDENDUM.md section 3).
 *
 * Cases, each repeated N times on one thread (the "guardian" side):
 *   SEND   the server never calls MsgReceive (client stays SEND-blocked)
 *   REPLY  the server receives and never replies, on a channel WITHOUT
 *          _NTO_CHF_UNBLOCK (the arm's design): the kernel must release the client
 *   UNBLK  the same on a channel WITH _NTO_CHF_UNBLOCK, where the docs say the
 *          server gets an unblock pulse and decides; the server replies 20 ms
 *          later, so the client is expected to wait ~20 ms, not 2 ms. This shows
 *          why the arm's channel must not set that flag.
 * Prints the tick (ClockPeriod) and clock_getres(), then min / median / 99.9th
 * percentile / max of the measured time from arming the timeout to MsgSend's
 * return. QEMU numbers prove function only, never timing.
 *
 * Pass condition, chosen by the argument --timing=strict|function (default strict):
 *   strict   (Pi 5, unchanged): SEND and REPLY always fail with ETIMEDOUT, never
 *            earlier than 2 ms, never later than 2 ms + 3 ticks.
 *   function (QEMU): SEND and REPLY always fail with ETIMEDOUT, never earlier than
 *            2 ms, never later than FUNCTION_CEILING_NS. Samples later than
 *            2 ms + 3 ticks are counted and reported, not failed. Changed for QEMU
 *            after observed failures: 6.314 ms in evidence/runs/2026-11-15-qemu/
 *            run-151353.log, and 5.081 / 5.331 ms in 2 of 50 runs in
 *            fr-g9-repeat-152521.log (QEMU guest pauses on the host; QEMU proves
 *            function, never timing).
 *   Both: UNBLK returns only after the server's delayed reply.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#define N_SAMPLES      1000
#define N_UNBLK        20
#define TIMEOUT_NS     2000000ull     /* the proposed guardian -> arm send timeout */
#define UNBLK_DELAY_NS 20000000ull    /* server reply delay in the UNBLK case */
/* QEMU-only upper bound: far above every QEMU value seen (6.314 ms worst), and
 * still fails a timeout that does not fire as set. Not a timing claim. */
#define FUNCTION_CEILING_NS 20000000ull

typedef struct {
    int      chid;
    int      reply_after_unblock;     /* UNBLK case */
} server_arg_t;

static uint64_t g_cps;                /* ClockCycles() per second */

static uint64_t cycles_to_ns(uint64_t c)
{
    return (uint64_t)((long double)c * 1e9L / (long double)g_cps);
}

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Receive forever; never reply, except in the UNBLK case (reply 20 ms after the unblock pulse). */
static void *server(void *p)
{
    const server_arg_t *a = (const server_arg_t *)p;
    union { struct _pulse pulse; char buf[64]; } msg;

    for (;;) {
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html */
        const rcvid_t rcvid = MsgReceive(a->chid, &msg, sizeof msg, NULL);
        if (rcvid == -1) {
            return NULL;
        }
        if (rcvid == 0 && a->reply_after_unblock && msg.pulse.code == _PULSE_CODE_UNBLOCK) {
            const struct timespec d = { 0, (long)UNBLK_DELAY_NS };
            const rcvid_t blocked = (rcvid_t)msg.pulse.value.sival_long;
            nanosleep(&d, NULL);
            /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html */
            (void)MsgError(blocked, EINTR);
        }
        /* otherwise: hold the message, never reply */
    }
}

static int timed_send(int coid, uint64_t *elapsed_ns, int *err)
{
    char out[16] = { 0 }, in[16];
    const uint64_t timeout = TIMEOUT_NS;
    uint64_t t0, t1;
    long rc;

    t0 = ClockCycles();
    /* Armed immediately before MsgSend(): it applies to the next kernel call only.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html */
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL) == -1) {
        *err = errno;
        return -1;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html */
    rc = MsgSend(coid, out, sizeof out, in, sizeof in);
    *err = (rc == -1) ? errno : 0;
    t1 = ClockCycles();
    *elapsed_ns = cycles_to_ns(t1 - t0);
    return 0;
}

static void report(const char *name, uint64_t *v, int n)
{
    const int p999 = (n * 999) / 1000 < n ? (n * 999) / 1000 : n - 1;
    qsort(v, (size_t)n, sizeof v[0], cmp_u64);
    printf("info %-5s n=%d  min %.3f ms  median %.3f ms  p99.9 %.3f ms  max %.3f ms\n", name, n,
           (double)v[0] / 1e6, (double)v[n / 2] / 1e6, (double)v[p999] / 1e6, (double)v[n - 1] / 1e6);
}

/* Run one case; returns 1 if every sample met its condition. */
/* Samples above hi_ns fail. Samples above soft_hi_ns (when below hi_ns) are only
 * counted and reported. */
static int run_case(const char *name, unsigned chflags, int with_server, int reply_after_unblock,
                    int n, uint64_t lo_ns, uint64_t hi_ns, uint64_t soft_hi_ns, int expect_err,
                    uint64_t *samples)
{
    static server_arg_t arg;
    pthread_t tid;
    int ok = 1;
    int over_soft = 0;
    int coid;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html */
    arg.chid = ChannelCreate(chflags);
    arg.reply_after_unblock = reply_after_unblock;
    if (arg.chid == -1) {
        printf("FAIL %s ChannelCreate: %s\n", name, strerror(errno));
        return 0;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html */
    coid = ConnectAttach(0, 0, arg.chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        printf("FAIL %s ConnectAttach: %s\n", name, strerror(errno));
        return 0;
    }
    if (with_server) {
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_create.html */
        if (pthread_create(&tid, NULL, server, &arg) != 0) {
            printf("FAIL %s pthread_create\n", name);
            return 0;
        }
        (void)pthread_detach(tid);
    }
    for (int i = 0; i < n; i++) {                 /* bound: n */
        int err = 0;
        if (timed_send(coid, &samples[i], &err) != 0) {
            printf("FAIL %s TimerTimeout: %s\n", name, strerror(err));
            return 0;
        }
        if (err != expect_err || samples[i] < lo_ns || samples[i] > hi_ns) {
            if (ok) {
                printf("  %s sample %d: err %d (%s), %.3f ms, expected err %d within [%.3f, %.3f] ms\n",
                       name, i, err, strerror(err), (double)samples[i] / 1e6, expect_err,
                       (double)lo_ns / 1e6, (double)hi_ns / 1e6);
            }
            ok = 0;
        } else if (samples[i] > soft_hi_ns) {
            printf("  %s sample %d: %.3f ms, above %.3f ms (reported, not failed in function mode)\n",
                   name, i, (double)samples[i] / 1e6, (double)soft_hi_ns / 1e6);
            over_soft++;
        }
    }
    report(name, samples, n);
    if (soft_hi_ns < hi_ns) {
        printf("info %-5s %d of %d samples above %.3f ms (2 ms + 3 ticks); pass limit %.3f ms\n", name,
               over_soft, n, (double)soft_hi_ns / 1e6, (double)hi_ns / 1e6);
    }
    return ok;
}

static int fr_g9_send_timeout_fires_when_server_never_receives(uint64_t *samples, uint64_t tick_ns, int strict)
{
    const uint64_t three_ticks = TIMEOUT_NS + 3u * tick_ns;
    const int ok = run_case("SEND", _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT, 0, 0, N_SAMPLES,
                            TIMEOUT_NS, strict ? three_ticks : FUNCTION_CEILING_NS, three_ticks, ETIMEDOUT,
                            samples);
    printf("%s fr_g9_send_timeout_fires_when_server_never_receives\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int fr_g9_send_timeout_fires_when_server_never_replies(uint64_t *samples, uint64_t tick_ns, int strict)
{
    const uint64_t three_ticks = TIMEOUT_NS + 3u * tick_ns;
    const int ok = run_case("REPLY", _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT, 1, 0, N_SAMPLES,
                            TIMEOUT_NS, strict ? three_ticks : FUNCTION_CEILING_NS, three_ticks, ETIMEDOUT,
                            samples);
    printf("%s fr_g9_send_timeout_fires_when_server_never_replies\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* With _NTO_CHF_UNBLOCK the client waits for the server: >= 20 ms here, ending in EINTR. */
static int fr_g9_unblock_flag_lets_server_delay_timed_out_client(uint64_t *samples)
{
    const int ok = run_case("UNBLK", _NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT | _NTO_CHF_UNBLOCK, 1, 1,
                            N_UNBLK, UNBLK_DELAY_NS, UNBLK_DELAY_NS + 50000000ull,
                            UNBLK_DELAY_NS + 50000000ull, EINTR, samples);
    printf("%s fr_g9_unblock_flag_lets_server_delay_timed_out_client\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main(int argc, char **argv)
{
    static uint64_t samples[N_SAMPLES];
    struct _clockperiod period;
    struct timespec res;
    uint64_t tick_ns;
    int failures = 0;
    int strict = 1;                   /* default: the strict (Pi 5) rule */

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && strcmp(argv[1], "--timing=function") == 0) {
        strict = 0;
    } else if (argc == 2 && strcmp(argv[1], "--timing=strict") == 0) {
        strict = 1;
    } else if (argc != 1) {
        printf("FAIL usage: test_send_timeout [--timing=strict|function]\n");
        return 1;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/syspage_entry.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockcycles.html */
    g_cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    /* Read the tick without changing it (new = NULL).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockperiod.html */
    if (ClockPeriod(CLOCK_MONOTONIC, NULL, &period, 0) == -1) {
        printf("FAIL ClockPeriod: %s\n", strerror(errno));
        return 1;
    }
    tick_ns = period.nsec;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_getres.html */
    if (clock_getres(CLOCK_MONOTONIC, &res) != 0) {
        printf("FAIL clock_getres: %s\n", strerror(errno));
        return 1;
    }
    printf("info clock period (tick) %u ns; clock_getres(CLOCK_MONOTONIC) %ld s %ld ns; ClockCycles %llu /s\n",
           (unsigned)period.nsec, (long)res.tv_sec, res.tv_nsec, (unsigned long long)g_cps);
    printf("info requested timeout %.3f ms\n", (double)TIMEOUT_NS / 1e6);
    printf("info timing %s: SEND and REPLY fail above %.3f ms\n", strict ? "strict (2 ms + 3 ticks)" : "function (QEMU)",
           (double)(strict ? TIMEOUT_NS + 3u * tick_ns : FUNCTION_CEILING_NS) / 1e6);

    failures += !fr_g9_send_timeout_fires_when_server_never_receives(samples, tick_ns, strict);
    failures += !fr_g9_send_timeout_fires_when_server_never_replies(samples, tick_ns, strict);
    failures += !fr_g9_unblock_flag_lets_server_delay_timed_out_client(samples);

    printf("RESULT %s test_send_timeout\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
