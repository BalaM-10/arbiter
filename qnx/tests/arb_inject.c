/*
 * arb-inject: the fault bench's on-target client (FR-B1..B3). Untrusted: it is
 * a planner stand-in, runs at the default user priority (10, below every trusted
 * process) and reaches the safety core only through /dev/arbiter, like the
 * gateway will.
 *
 * It plays a timed command script from stdin, generated on the host by
 * `arbiter bench run`, and records two things:
 *   - every verdict the guardian returned (V lines), and
 *   - every arm telemetry sample written during the run (T lines), read from
 *     the arm's read-only shared memory.
 * The host bench judges limits, keep-out and speed from the T lines with its
 * own kinematics, never from the verdicts.
 *
 * Nothing is printed until the script ends: records stay in static memory, so
 * a slow SSH pipe can never delay a command (a delay past the silence timeout
 * would change the scenario).
 *
 * Script, one step per line, times in ms from the start, non-decreasing:
 *   C <t_ms> <seq> <q0_urad> <q1_urad> <duration_ms>   submit a command
 *   S <t_ms>                                           record the guardian status
 *   E <t_ms>                                           end of the run
 *   # ...                                              comment
 *
 * Output (all integers):
 *   ARB-INJECT 1
 *   INFO telem_version <v> cycles_per_sec <n> slots <n>
 *   COUNTERS <begin|end> rejected <n> lease_expiries <n> disconnects <n> wd_brakes <n> wd_lost <n> bound_pid <n>
 *     wd_lost_cause <n>   (ARB_WD_LOST_* of the arm's first loss of the watchdog, 0 if none)
 *   WD <begin|end> trips <n> rejected <n> hb_guardian <n> hb_arm <n> deliver_errors <n> beat_errors <n>
 *     last_role <n> last_cause <n> last_pid <n>   (the watchdog's last trip: ARB_WD_ROLE_*, ARB_WD_CAUSE_*)
 *     (the watchdog's own counters; heartbeats advance only while the guardian,
 *     the arm and the watchdog are all running)
 *   V <t_send_ns> <t_reply_ns> <telem_head> <seq> <rc> <verdict> <rule> <state> <flags>
 *     <target0> <target1> <duration_ms> <requested_ms>
 *   S <t_ns> <state> <stop_cause> <decisions> <arm_faults> <ticks> <bad_messages> <watchdog_trips>
 *     <foreign_pulses> <watchdog_lost> <watchdog_lost_cause>
 *   T <index> <t_ns> <t_cycles> <pos0> <pos1> <vel0> <vel1> <segment> <state> <lease_ok>
 *   GAP <first_index> <count>              samples the reader lost (the bench fails on any)
 *   DONE samples <n> gaps <n> verdicts <n>
 *
 * Exit: 0 run complete, 2 bad script, 3 /dev/arbiter, 4 telemetry, 5 record overflow.
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

#include "arb_contract.h"
#include "arb_devctl.h"
#include "arb_ids.h"
#include "arb_telemetry.h"
#include "arb_watchdog_proto.h"

#define MAX_STEPS     2048u
#define MAX_SAMPLES   40000u      /* 40 s at 1 kHz */
#define MAX_GAPS      256u
#define PREROLL       50u         /* samples kept from before the first step */
#define DRAIN_MS      20          /* telemetry read period; the ring holds 8 s */

typedef struct {
    char     kind;                /* 'C', 'S' or 'E' */
    int64_t  t_ms;
    uint32_t seq;
    int32_t  q[ARB_JOINTS];
    uint32_t duration_ms;
} step_t;

typedef struct {
    int64_t  t_send_ns, t_reply_ns;
    uint64_t head;
    int      rc;
    arb_verdict_wire_t w;
} vrec_t;

typedef struct {
    int64_t      t_ns;
    arb_status_t st;
} srec_t;

typedef struct {
    uint64_t first, count;
} gap_t;

static step_t             g_steps[MAX_STEPS];
static vrec_t             g_v[MAX_STEPS];
static srec_t             g_s[MAX_STEPS];
static arb_telem_sample_t g_samples[MAX_SAMPLES];
static gap_t              g_gaps[MAX_GAPS];
static unsigned           g_nsteps, g_nv, g_ns, g_nsamples, g_ngaps;
static int                g_overflow;
static uint64_t           g_next;
static const arb_telem_t *g_tm;
static const arb_wd_status_t *g_wd;

/* Counters snapshotted at the start and the end of the run, printed afterwards. */
typedef struct {
    uint64_t rejected, lease_expiries, disconnects, wd_brakes, wd_lost, wd_lost_cause;
    int64_t  bound_pid;
    uint64_t wd_trips, wd_rejected, hb[3], wd_deliver_errors, wd_beat_errors;
    uint32_t wd_last_role, wd_last_cause;
    int64_t  wd_last_pid;
} snap_t;

static int64_t now_ns(void)
{
    struct timespec ts;

    /* Same clock as the arm's t_ns.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html */
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

static void sleep_until_ns(int64_t t)
{
    struct timespec ts = { (time_t)(t / 1000000000), (long)(t % 1000000000) };

    /* Absolute sleep on the monotonic clock; returns the error number itself.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_nanosleep.html */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static uint64_t head(void)
{
    return atomic_load_explicit(&((arb_telem_t *)g_tm)->head, memory_order_acquire);
}

static void add_gap(uint64_t first, uint64_t count)
{
    if (g_ngaps < MAX_GAPS) {
        g_gaps[g_ngaps].first = first;
        g_gaps[g_ngaps].count = count;
        g_ngaps++;
    } else {
        g_overflow = 1;
    }
}

/* Copy every sample written since the last call (arb_telemetry.h reader rule). */
static void drain(void)
{
    const uint64_t h = head();

    if (h - g_next > ARB_TELEM_SLOTS - 64u) {     /* fell behind: those slots may be reused */
        const uint64_t keep_from = h - (ARB_TELEM_SLOTS - 64u);
        add_gap(g_next, keep_from - g_next);
        g_next = keep_from;
    }
    for (; g_next < h; g_next++) {                /* bound: < ARB_TELEM_SLOTS per call */
        arb_telem_sample_t c = g_tm->s[g_next % ARB_TELEM_SLOTS];
        uint64_t h2;

        atomic_thread_fence(memory_order_acquire);
        h2 = atomic_load_explicit(&((arb_telem_t *)g_tm)->head, memory_order_relaxed);
        if (c.index != g_next || g_next + ARB_TELEM_SLOTS <= h2) {
            add_gap(g_next, 1u);
            continue;
        }
        if (g_nsamples >= MAX_SAMPLES) {
            g_overflow = 1;
            continue;
        }
        g_samples[g_nsamples++] = c;
    }
}

static void snapshot(snap_t *s)
{
    arb_telem_t *t = (arb_telem_t *)g_tm;
    arb_wd_status_t *w = (arb_wd_status_t *)g_wd;

    s->rejected = atomic_load(&t->rejected);
    s->lease_expiries = atomic_load(&t->lease_expiries);
    s->disconnects = atomic_load(&t->disconnects);
    s->wd_brakes = atomic_load(&t->wd_brakes);
    s->wd_lost = atomic_load(&t->wd_lost);
    s->wd_lost_cause = atomic_load(&t->wd_lost_cause);
    s->bound_pid = atomic_load(&t->bound_pid);
    s->wd_trips = atomic_load(&w->trips);
    s->wd_rejected = atomic_load(&w->rejected);
    for (int r = 0; r < 3; r++) {                     /* bound: 3 roles */
        s->hb[r] = atomic_load(&w->heartbeats[r]);
    }
    s->wd_deliver_errors = atomic_load(&w->deliver_errors);
    s->wd_beat_errors = atomic_load(&w->beat_errors);
    s->wd_last_role = atomic_load(&w->last_role);
    s->wd_last_cause = atomic_load(&w->last_cause);
    s->wd_last_pid = atomic_load(&w->last_pid);
}

static void print_snapshot(const char *phase, const snap_t *s)
{
    printf("COUNTERS %s rejected %llu lease_expiries %llu disconnects %llu wd_brakes %llu wd_lost %llu "
           "bound_pid %lld wd_lost_cause %llu\n",
           phase, (unsigned long long)s->rejected, (unsigned long long)s->lease_expiries,
           (unsigned long long)s->disconnects, (unsigned long long)s->wd_brakes, (unsigned long long)s->wd_lost,
           (long long)s->bound_pid, (unsigned long long)s->wd_lost_cause);
    printf("WD %s trips %llu rejected %llu hb_guardian %llu hb_arm %llu deliver_errors %llu beat_errors %llu "
           "last_role %u last_cause %u last_pid %lld\n",
           phase, (unsigned long long)s->wd_trips, (unsigned long long)s->wd_rejected,
           (unsigned long long)s->hb[ARB_WD_ROLE_GUARDIAN], (unsigned long long)s->hb[ARB_WD_ROLE_ARM],
           (unsigned long long)s->wd_deliver_errors, (unsigned long long)s->wd_beat_errors,
           (unsigned)s->wd_last_role, (unsigned)s->wd_last_cause, (long long)s->wd_last_pid);
}

static int parse_script(void)
{
    char line[256];
    int64_t last = 0;
    int ended = 0;

    while (fgets(line, sizeof line, stdin) != NULL) {
        step_t s;
        long long t = 0;
        unsigned long seq = 0, dur = 0;
        long q0 = 0, q1 = 0;

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (ended || g_nsteps >= MAX_STEPS) {
            fprintf(stderr, "arb-inject: too many steps, or steps after E\n");
            return -1;
        }
        memset(&s, 0, sizeof s);
        s.kind = line[0];
        if (s.kind == 'C' && sscanf(line + 1, "%lld %lu %ld %ld %lu", &t, &seq, &q0, &q1, &dur) == 5) {
            s.seq = (uint32_t)seq;
            s.q[0] = (int32_t)q0;
            s.q[1] = (int32_t)q1;
            s.duration_ms = (uint32_t)dur;
        } else if ((s.kind == 'S' || s.kind == 'E') && sscanf(line + 1, "%lld", &t) == 1) {
            ended = s.kind == 'E';
        } else {
            fprintf(stderr, "arb-inject: bad script line: %s", line);
            return -1;
        }
        if (t < last || t > 600000) {
            fprintf(stderr, "arb-inject: step times must be non-decreasing and under 600 s\n");
            return -1;
        }
        last = t;
        s.t_ms = t;
        g_steps[g_nsteps++] = s;
    }
    if (!ended) {
        fprintf(stderr, "arb-inject: script has no E step\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    int fd, tfd;
    snap_t begin, end;
    int64_t t0;

    if (parse_script() != 0) {
        return 2;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html */
    fd = open(ARB_DEV_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "arb-inject: open %s: %s\n", ARB_DEV_PATH, strerror(errno));
        return 3;
    }
    /* Read-only mapping of the arm's telemetry.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
    tfd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    if (tfd == -1) {
        fprintf(stderr, "arb-inject: shm_open %s: %s\n", ARB_TELEM_NAME, strerror(errno));
        return 4;
    }
    g_tm = mmap(NULL, sizeof(arb_telem_t), PROT_READ, MAP_SHARED, tfd, 0);
    if (g_tm == MAP_FAILED || g_tm->magic != ARB_TELEM_MAGIC || g_tm->version != ARB_TELEM_VERSION) {
        fprintf(stderr, "arb-inject: telemetry missing or wrong version (want %u)\n", ARB_TELEM_VERSION);
        return 4;
    }
    tfd = shm_open(ARB_WD_STATUS_NAME, O_RDONLY, 0);
    g_wd = tfd == -1 ? MAP_FAILED : mmap(NULL, sizeof(arb_wd_status_t), PROT_READ, MAP_SHARED, tfd, 0);
    if (g_wd == MAP_FAILED || g_wd->magic != ARB_WD_STATUS_MAGIC) {
        fprintf(stderr, "arb-inject: watchdog status %s missing\n", ARB_WD_STATUS_NAME);
        return 4;
    }
    /* FR-G10: only the gateway's user may submit, so the bench drives the guardian
     * as that user. Set once here, outside the timed sends; started as root by
     * bench_scenario.sh, it never needs root again.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/seteuid.html */
    if (seteuid((uid_t)ARB_UID_GATEWAY) == -1) {
        fprintf(stderr, "arb-inject: seteuid %d: %s\n", ARB_UID_GATEWAY, strerror(errno));
        return 5;
    }

    printf("ARB-INJECT 1\n");
    printf("INFO telem_version %u cycles_per_sec %llu slots %u\n", g_tm->version,
           (unsigned long long)g_tm->cycles_per_sec, ARB_TELEM_SLOTS);
    snapshot(&begin);
    print_snapshot("begin", &begin);
    {
        const uint64_t h = head();
        g_next = h > PREROLL ? h - PREROLL : 0u;
    }

    t0 = now_ns();
    for (unsigned i = 0; i < g_nsteps; i++) {    /* bound: g_nsteps <= MAX_STEPS */
        const step_t *s = &g_steps[i];
        const int64_t due = t0 + s->t_ms * 1000000;

        for (;;) {                               /* bound: (due - now) / DRAIN_MS iterations */
            const int64_t n = now_ns();
            if (n >= due) {
                break;
            }
            sleep_until_ns(due - n > DRAIN_MS * 1000000 ? n + DRAIN_MS * 1000000 : due);
            drain();
        }
        if (s->kind == 'C') {
            arb_submit_t u;
            vrec_t *v = &g_v[g_nv++];

            memset(&u, 0, sizeof u);
            u.cmd.version = ARB_PROTO_VERSION;
            u.cmd.seq = s->seq;
            u.cmd.target_urad[0] = s->q[0];
            u.cmd.target_urad[1] = s->q[1];
            u.cmd.duration_ms = s->duration_ms;
            v->head = head();
            v->t_send_ns = now_ns();
            u.cmd.sender_ts_ns = (uint64_t)v->t_send_ns;
            /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html */
            v->rc = devctl(fd, ARB_DCMD_SUBMIT, &u, sizeof u, NULL);
            v->t_reply_ns = now_ns();
            v->w = u.verdict;
        } else if (s->kind == 'S') {
            srec_t *r = &g_s[g_ns++];

            memset(&r->st, 0, sizeof r->st);
            r->t_ns = now_ns();
            (void)devctl(fd, ARB_DCMD_GET_STATUS, &r->st, sizeof r->st, NULL);
        }
        drain();
    }
    drain();
    snapshot(&end);

    for (unsigned i = 0; i < g_nv; i++) {
        const vrec_t *v = &g_v[i];
        printf("V %lld %lld %llu %u %d %u %u %u %u %d %d %u %u\n", (long long)v->t_send_ns,
               (long long)v->t_reply_ns, (unsigned long long)v->head, v->w.seq, v->rc,
               (unsigned)v->w.verdict, (unsigned)v->w.rule, (unsigned)v->w.state, (unsigned)v->w.flags,
               v->w.target_urad[0], v->w.target_urad[1], v->w.duration_ms, v->w.requested_ms);
    }
    for (unsigned i = 0; i < g_ns; i++) {
        const arb_status_t *st = &g_s[i].st;
        printf("S %lld %u %u %u %u %u %u %u %u %u %u\n", (long long)g_s[i].t_ns, (unsigned)st->state,
               (unsigned)st->stop_cause, st->decisions, st->arm_faults, st->ticks, st->bad_messages,
               st->watchdog_trips, st->foreign_pulses, (unsigned)st->watchdog_lost,
               (unsigned)st->watchdog_lost_cause);
    }
    for (unsigned i = 0; i < g_nsamples; i++) {
        const arb_telem_sample_t *c = &g_samples[i];
        printf("T %llu %lld %llu %d %d %d %d %u %u %u\n", (unsigned long long)c->index, (long long)c->t_ns,
               (unsigned long long)c->t_cycles, c->pos_urad[0], c->pos_urad[1], c->vel_urad_s[0],
               c->vel_urad_s[1], c->segment, (unsigned)c->state, (unsigned)c->lease_ok);
    }
    for (unsigned i = 0; i < g_ngaps; i++) {
        printf("GAP %llu %llu\n", (unsigned long long)g_gaps[i].first, (unsigned long long)g_gaps[i].count);
    }
    print_snapshot("end", &end);
    printf("DONE samples %u gaps %u verdicts %u\n", g_nsamples, g_ngaps, g_nv);
    return g_overflow ? 5 : 0;
}
