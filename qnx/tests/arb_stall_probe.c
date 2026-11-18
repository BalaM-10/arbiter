/*
 * arb-stall-probe: measures what a stall of the whole QEMU guest does to Arbiter
 * (midpoint review M3). A test tool, not trusted code: it runs as root at the
 * default priority (10) on the policy-off image, next to a running watchdog, arm
 * and guardian, and is driven by scripts/qemu-stall.sh on the host.
 *
 *   arb-stall-probe watch <seconds>   keep the arm moving; print every change of the
 *                                     supervision counters with its time; at the end,
 *                                     the arm's tick intervals and the submits allowed
 *   arb-stall-probe rate <seconds>    the same moves for a long run; record every arm
 *                                     tick interval; after each SAFE_STOP, an operator
 *                                     reset (as user 800, like arb-ctl reset), so later
 *                                     stalls are counted too
 *
 * Moves: every 150 ms a 150 ms move, alternating between home and (-37400, 74800)
 * urad (the first step of bench F1), submitted as the gateway's user (FR-G10).
 *
 * Output (one record per line):
 *   EVENT <t_ms> <counter> <old> <new>      watch: any change; rate: trips, losses, state
 *   STALL <t_ms> <interval_ms>              an arm tick interval of 10 ms or more
 *   RESET <t_ms> <rc> <state after>         rate: the operator reset after a SAFE_STOP
 *   SUMMARY ...                             counts (see the end of main)
 *
 * QEMU proves function; the times are QEMU measurements, not claims.
 * Exit: 0 run complete, 1 setup failed, 2 usage.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/neutrino.h>

#include "arb_contract.h"
#include "arb_devctl.h"
#include "arb_ids.h"
#include "arb_telemetry.h"
#include "arb_watchdog_proto.h"
#include "arb_as_gateway.h"

#define MAX_SECONDS   3600
#define MOVE_MS       150
#define MAX_STALLS    200       /* STALL lines printed at most */
#define N_COUNTERS    16

static const arb_telem_t *g_tm;
static const arb_wd_status_t *g_wd;
static int g_fd = -1;
static uint64_t g_c0;

static uint64_t ld64(const _Atomic uint64_t *a) { return atomic_load((_Atomic uint64_t *)a); }
static uint32_t ld32(const _Atomic uint32_t *a) { return atomic_load((_Atomic uint32_t *)a); }

/* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockcycles.html */
static double cycles_ms(uint64_t c) { return (double)c * 1000.0 / (double)g_tm->cycles_per_sec; }
static double t_ms(void) { return cycles_ms(ClockCycles() - g_c0); }

static void ms_sleep(long ms)
{
    const struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static const char *const NAMES[N_COUNTERS] = {
    "arm.wd_lost", "arm.wd_lost_cause", "arm.wd_brakes", "arm.rejected", "arm.lease_expiries",
    "wd.trips", "wd.last_role", "wd.last_cause", "wd.rejected",
    "guardian.state", "guardian.stop_cause", "guardian.watchdog_lost", "guardian.watchdog_lost_cause",
    "guardian.watchdog_trips", "guardian.arm_faults", "guardian.resets",
};
/* In rate mode only these are printed as events (wd.rejected counts every refused
 * heartbeat of a tripped client, one line per millisecond). */
static const int RATE_EVENT[N_COUNTERS] = { 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 };

static void snapshot(uint64_t v[N_COUNTERS], arb_status_t *st)
{
    memset(st, 0, sizeof *st);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html */
    (void)devctl(g_fd, ARB_DCMD_GET_STATUS, st, sizeof *st, NULL);
    v[0] = ld64(&g_tm->wd_lost);
    v[1] = ld64(&g_tm->wd_lost_cause);
    v[2] = ld64(&g_tm->wd_brakes);
    v[3] = ld64(&g_tm->rejected);
    v[4] = ld64(&g_tm->lease_expiries);
    v[5] = ld64(&g_wd->trips);
    v[6] = ld32(&g_wd->last_role);
    v[7] = ld32(&g_wd->last_cause);
    v[8] = ld64(&g_wd->rejected);
    v[9] = st->state;
    v[10] = st->stop_cause;
    v[11] = st->watchdog_lost;
    v[12] = st->watchdog_lost_cause;
    v[13] = st->watchdog_trips;
    v[14] = st->arm_faults;
    v[15] = st->resets;
}

/* Arm tick intervals, from every telemetry sample (the ring holds 8 s; read every pass). */
typedef struct {
    uint64_t next;              /* next sample index to read */
    uint64_t last_cycles;       /* ClockCycles() of the previous sample, 0 before the first */
    uint64_t intervals, ge2, ge5, ge10, ge20, ge50, burst, lost;
    double   largest;
    int      printed;
} tick_stats_t;

static void drain(tick_stats_t *t)
{
    const uint64_t head = atomic_load_explicit(&((arb_telem_t *)g_tm)->head, memory_order_acquire);

    if (head - t->next > ARB_TELEM_SLOTS - 64u) {     /* fell behind: those slots may be reused */
        t->lost += head - (ARB_TELEM_SLOTS - 64u) - t->next;
        t->next = head - (ARB_TELEM_SLOTS - 64u);
        t->last_cycles = 0;
    }
    for (; t->next < head; t->next++) {               /* bound: < ARB_TELEM_SLOTS per call */
        arb_telem_sample_t s;
        double ms;

        if (!arb_telem_get(g_tm, t->next, &s)) {      /* overwritten while we read it */
            t->lost++;
            t->last_cycles = 0;
            continue;
        }
        if (t->last_cycles != 0) {
            ms = cycles_ms(s.t_cycles - t->last_cycles);
            t->intervals++;
            t->ge2 += ms >= 2.0;
            t->ge5 += ms >= 5.0;
            t->ge10 += ms >= 10.0;
            t->ge20 += ms >= 20.0;
            t->ge50 += ms >= 50.0;
            t->burst += ms < 0.1;
            if (ms > t->largest) {
                t->largest = ms;
            }
            if (ms >= 10.0 && t->printed < MAX_STALLS) {
                t->printed++;
                printf("STALL %.3f %.3f\n", cycles_ms(s.t_cycles - g_c0), ms);
            }
        }
        t->last_cycles = s.t_cycles;
    }
}

int main(int argc, char **argv)
{
    const char *mode = argc == 3 ? argv[1] : "";
    const int rate = strcmp(mode, "rate") == 0;
    const long secs = argc == 3 ? strtol(argv[2], NULL, 10) : 0;
    uint64_t a[N_COUNTERS], b[N_COUNTERS];
    tick_stats_t ticks;
    arb_status_t st;
    uint32_t seq;
    long slot_done = -1;
    unsigned long allowed = 0, denied = 0, safe_stops = 0, resets_ok = 0, resets_failed = 0;
    double probe_largest = 0.0, prev = 0.0;
    int fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if ((!rate && strcmp(mode, "watch") != 0) || secs < 1 || secs > MAX_SECONDS) {
        printf("usage: arb-stall-probe watch|rate <seconds 1..%d>\n", MAX_SECONDS);
        return 2;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
    fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    g_tm = fd == -1 ? MAP_FAILED : mmap(NULL, sizeof *g_tm, PROT_READ, MAP_SHARED, fd, 0);
    if (fd != -1) {
        (void)close(fd);
    }
    fd = shm_open(ARB_WD_STATUS_NAME, O_RDONLY, 0);
    g_wd = fd == -1 ? MAP_FAILED : mmap(NULL, sizeof *g_wd, PROT_READ, MAP_SHARED, fd, 0);
    if (fd != -1) {
        (void)close(fd);
    }
    g_fd = open(ARB_DEV_PATH, O_RDWR);
    if (g_tm == MAP_FAILED || g_wd == MAP_FAILED || g_fd == -1) {
        printf("setup failed: telemetry, watchdog status or %s: %s\n", ARB_DEV_PATH, strerror(errno));
        return 1;
    }
    memset(&ticks, 0, sizeof ticks);
    ticks.next = atomic_load_explicit(&((arb_telem_t *)g_tm)->head, memory_order_acquire);
    snapshot(a, &st);
    seq = st.has_seq ? st.last_seq : 0u;
    g_c0 = ClockCycles();
    printf("START mode %s seconds %ld telemetry_head %llu\n", mode, secs, (unsigned long long)ticks.next);

    for (long i = 0; i < secs * 2000L; i++) {         /* bound: 2 passes per ms of run time at most */
        const double now = t_ms();
        const long slot = (long)(now / MOVE_MS);

        if (now >= (double)secs * 1000.0) {
            break;
        }
        if (now - prev > probe_largest && i > 0) {
            probe_largest = now - prev;                /* the probe's own longest pass */
        }
        prev = now;
        if (slot != slot_done) {
            arb_submit_t u;
            const int home = (slot % 2) == 0;
            int rc;

            slot_done = slot;
            memset(&u, 0, sizeof u);
            u.cmd.version = ARB_PROTO_VERSION;
            u.cmd.seq = ++seq;
            u.cmd.target_urad[0] = home ? 0 : -37400;
            u.cmd.target_urad[1] = home ? 0 : 74800;
            u.cmd.duration_ms = MOVE_MS;
            rc = arb_devctl_gw(g_fd, ARB_DCMD_SUBMIT, &u, sizeof u);
            if (rc == EOK && u.verdict.verdict == ARB_VERDICT_ALLOW) {
                allowed++;
            } else {
                denied++;
            }
        }
        drain(&ticks);
        snapshot(b, &st);
        for (int k = 0; k < N_COUNTERS; k++) {        /* bound: N_COUNTERS */
            if (a[k] != b[k] && (!rate || RATE_EVENT[k])) {
                printf("EVENT %.3f %s %llu %llu\n", now, NAMES[k], (unsigned long long)a[k],
                       (unsigned long long)b[k]);
            }
        }
        if (b[9] == ARB_STATE_SAFE_STOP && a[9] != ARB_STATE_SAFE_STOP) {
            safe_stops++;
        }
        memcpy(a, b, sizeof a);
        if (rate && st.state == ARB_STATE_SAFE_STOP) {
            /* The operator's reset, as arb-ctl reset does it (FR-G5), after the
             * watchdog has seen the clients again for a moment. */
            int rc;

            ms_sleep(100);
            rc = arb_devctl_as(ARB_UID_OPERATOR, g_fd, ARB_DCMD_RESET, NULL, 0);
            snapshot(b, &st);
            printf("RESET %.3f %d %u\n", t_ms(), rc, (unsigned)st.state);
            if (rc == EOK) {
                resets_ok++;
            } else {
                resets_failed++;
            }
            memcpy(a, b, sizeof a);
        }
        ms_sleep(1);
    }
    drain(&ticks);
    printf("SUMMARY seconds %ld intervals %llu ge2ms %llu ge5ms %llu ge10ms %llu ge20ms %llu ge50ms %llu "
           "below0.1ms %llu largest_ms %.3f samples_lost %llu\n", secs,
           (unsigned long long)ticks.intervals, (unsigned long long)ticks.ge2, (unsigned long long)ticks.ge5,
           (unsigned long long)ticks.ge10, (unsigned long long)ticks.ge20, (unsigned long long)ticks.ge50,
           (unsigned long long)ticks.burst, ticks.largest, (unsigned long long)ticks.lost);
    printf("SUMMARY submits_allowed %lu submits_denied %lu safe_stops %lu resets_ok %lu resets_failed %lu "
           "wd_trips %llu probe_longest_pass_ms %.3f\n", allowed, denied, safe_stops, resets_ok, resets_failed,
           (unsigned long long)ld64(&g_wd->trips), probe_largest);
    printf("SUMMARY end");
    for (int k = 0; k < N_COUNTERS; k++) {            /* bound: N_COUNTERS */
        printf(" %s=%llu", NAMES[k], (unsigned long long)a[k]);
    }
    printf("\n");
    return 0;
}
