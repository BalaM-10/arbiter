/*
 * Host test of the two shared-memory rings (review M2-J8), built with
 * ThreadSanitizer by `make -C qnx host-rings` and run by scripts/host-checks.sh.
 * It uses the very functions the arm, the guardian, the logger and the bench use
 * (arb_telem_put/get, arb_dec_put/get in arb_telemetry.h and arb_decisions.h).
 *
 * One writer thread fills each ring as fast as it can, for many laps; a reader
 * thread at the same time reads the newest slots and the oldest ones (the slots
 * the writer is about to overwrite). Every field of record n is derived from n,
 * so a torn copy is visible: the reader must never accept a record whose fields
 * disagree. ThreadSanitizer must also report no data race: the C11 memory model
 * is what the Pi 5's weaker memory ordering holds the code to, where x86 would
 * forgive a missing fence.
 *
 * Output: PASS/FAIL per test with the counts; exit 0 only if both pass. A race
 * report from ThreadSanitizer makes the process exit 66 (its default).
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arb_decisions.h"
#include "arb_telemetry.h"

#define TELEM_WRITES 200000u           /* about 24 laps of the telemetry ring */
#define DEC_WRITES   50000u            /* about 49 laps of the decision ring */
#define WINDOW       64u               /* slots read at each end of the ring per pass */

static arb_telem_t    g_t;
static arb_dec_ring_t g_d;
static atomic_int     g_done;

/* ------------------------------------------------------------- telemetry */

static void telem_fill(arb_telem_sample_t *s, uint64_t n)
{
    memset(s, 0, sizeof *s);
    s->t_cycles = n * 7u + 1u;
    s->t_ns = (int64_t)(n * 3u);
    s->pos_urad[0] = (int32_t)(n & 0x3fffffffu);
    s->pos_urad[1] = -(int32_t)(n & 0x3fffffffu);
    s->vel_urad_s[0] = (int32_t)((n ^ 0x5a5au) & 0x3fffffffu);
    s->vel_urad_s[1] = (int32_t)((n * 5u) & 0x3fffffffu);
    s->segment = (uint32_t)n;
    s->state = (uint8_t)(n & 3u);
    s->lease_ok = (uint8_t)(n & 1u);
}

static int telem_consistent(const arb_telem_sample_t *s, uint64_t n)
{
    arb_telem_sample_t want;

    telem_fill(&want, n);
    want.index = n;
    return memcmp(s, &want, sizeof want) == 0;
}

static void *telem_writer(void *arg)
{
    arb_telem_sample_t s;

    for (uint64_t n = 0; n < TELEM_WRITES; n++) {            /* bound: TELEM_WRITES */
        telem_fill(&s, n);
        arb_telem_put(&g_t, &s);
    }
    atomic_store(&g_done, 1);
    return arg;
}

typedef struct {
    uint64_t accepted, dropped, torn;
} counts_t;

static void *telem_reader(void *arg)
{
    counts_t *c = arg;

    while (!atomic_load(&g_done)) {                           /* bound: ends with the writer */
        const uint64_t h = arb_telem_head(&g_t);
        const uint64_t ends[2] = { h > WINDOW ? h - WINDOW : 0,
                                   h > ARB_TELEM_SLOTS ? h - ARB_TELEM_SLOTS : 0 };

        for (int e = 0; e < 2; e++) {                         /* bound: 2 ends */
            for (uint64_t i = ends[e]; i < ends[e] + WINDOW && i < h; i++) {   /* bound: WINDOW */
                arb_telem_sample_t s;
                if (!arb_telem_get(&g_t, i, &s)) {
                    c->dropped++;
                } else if (telem_consistent(&s, i)) {
                    c->accepted++;
                } else {
                    c->torn++;
                }
            }
        }
    }
    return arg;
}

/* -------------------------------------------------------------- decisions */

static void dec_fill(arb_dec_record_t *r, uint64_t n)
{
    memset(r, 0, sizeof *r);
    r->t_ns = (int64_t)(n * 11u);
    r->t_cycles = n * 13u + 5u;
    r->latency_cycles = n & 0xffffu;
    r->cmd.version = 1;
    r->cmd.seq = (uint32_t)n;
    r->cmd.sender_ts_ns = n * 17u;
    r->cmd.target_urad[0] = (int32_t)(n & 0xfffffu);
    r->cmd.target_urad[1] = -(int32_t)(n & 0xfffffu);
    r->cmd.duration_ms = (uint32_t)(n % 5000u) + 1u;
    r->verdict.seq = (uint32_t)n;
    r->verdict.verdict = (uint8_t)(n % 3u + 1u);
    r->verdict.rule = (uint8_t)(n % 10u);
    r->verdict.duration_ms = (uint32_t)(n % 7000u);
}

static int dec_consistent(const arb_dec_record_t *r, uint64_t n)
{
    arb_dec_record_t want;

    dec_fill(&want, n);
    want.index = n;
    return memcmp(r, &want, sizeof want) == 0;
}

static void *dec_writer(void *arg)
{
    arb_dec_record_t r;

    for (uint64_t n = 0; n < DEC_WRITES; n++) {               /* bound: DEC_WRITES */
        dec_fill(&r, n);
        arb_dec_put(&g_d, &r);
    }
    atomic_store(&g_done, 1);
    return arg;
}

static void *dec_reader(void *arg)
{
    counts_t *c = arg;

    while (!atomic_load(&g_done)) {                           /* bound: ends with the writer */
        const uint64_t h = arb_dec_head(&g_d);
        const uint64_t ends[2] = { h > WINDOW ? h - WINDOW : 0,
                                   h > ARB_DEC_SLOTS ? h - ARB_DEC_SLOTS : 0 };

        for (int e = 0; e < 2; e++) {                         /* bound: 2 ends */
            for (uint64_t i = ends[e]; i < ends[e] + WINDOW && i < h; i++) {   /* bound: WINDOW */
                arb_dec_record_t r;
                if (!arb_dec_get(&g_d, i, &r)) {
                    c->dropped++;
                } else if (dec_consistent(&r, i)) {
                    c->accepted++;
                } else {
                    c->torn++;
                }
            }
        }
    }
    return arg;
}

/* ------------------------------------------------------------------ tests */

static int run(void *(*writer)(void *), void *(*reader)(void *), counts_t *c)
{
    pthread_t w, r;

    memset(c, 0, sizeof *c);
    atomic_store(&g_done, 0);
    if (pthread_create(&r, NULL, reader, c) != 0) {
        return -1;
    }
    if (pthread_create(&w, NULL, writer, NULL) != 0) {
        atomic_store(&g_done, 1);
        (void)pthread_join(r, NULL);
        return -1;
    }
    (void)pthread_join(w, NULL);
    (void)pthread_join(r, NULL);
    return 0;
}

static int fr_a6_telemetry_reader_never_accepts_a_torn_sample(void)
{
    counts_t c;
    const int ok = run(telem_writer, telem_reader, &c) == 0 && c.torn == 0 && c.accepted > 0;

    printf("%s fr_a6_telemetry_reader_never_accepts_a_torn_sample (%u writes: %llu accepted, %llu dropped, "
           "%llu torn accepted)\n", ok ? "PASS" : "FAIL", TELEM_WRITES, (unsigned long long)c.accepted,
           (unsigned long long)c.dropped, (unsigned long long)c.torn);
    return ok;
}

static int fr_g8_decision_reader_never_accepts_a_torn_record(void)
{
    counts_t c;
    const int ok = run(dec_writer, dec_reader, &c) == 0 && c.torn == 0 && c.accepted > 0;

    printf("%s fr_g8_decision_reader_never_accepts_a_torn_record (%u writes: %llu accepted, %llu dropped, "
           "%llu torn accepted)\n", ok ? "PASS" : "FAIL", DEC_WRITES, (unsigned long long)c.accepted,
           (unsigned long long)c.dropped, (unsigned long long)c.torn);
    return ok;
}

int main(void)
{
    const int a = fr_a6_telemetry_reader_never_accepts_a_torn_sample();
    const int b = fr_g8_decision_reader_never_accepts_a_torn_record();

    printf("RESULT %s test_rings_host %d/2\n", a && b ? "PASS" : "FAIL", a + b);
    return a && b ? 0 : 1;
}
