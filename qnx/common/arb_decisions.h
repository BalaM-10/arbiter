/*
 * Decision ring (FR-G8, FR-E1, NFR-Q4): one record per command the guardian
 * decided, in shared memory, written only by the guardian before it replies,
 * and read only by the evidence logger (arb-logd). Single writer, lock-free, as
 * the arm's telemetry ring (arb_telemetry.h): the guardian never waits for a
 * reader; a reader that falls behind loses the oldest records and counts them
 * (FR-E5).
 *
 * Writer: write record head into slot head % ARB_DEC_SLOTS under the slot's
 * sequence lock (arb_seqlock.h), then publish head + 1 (release).
 * Reader: read head (acquire), copy a slot under its sequence lock and check its
 * index; a copy that was being written, or holds another record, is dropped (and
 * counted, FR-E5). Since version 2 a torn copy cannot be accepted on any C11
 * memory model (review M2-J8; before, it relied on x86's ordering).
 *
 * Access: mode 0640, group ARB_UID_LOGD (the logger's user and group ID), set by
 * the guardian while it is still root. The gateway's user cannot open it, so no
 * trusted memory is shared with the gateway (NFR-I1).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fchown.html
 */
#ifndef ARB_DECISIONS_H
#define ARB_DECISIONS_H

#include <stdatomic.h>
#include <stdint.h>

#include "arb_proto.h"
#include "arb_contract.h"
#include "arb_seqlock.h"

#define ARB_DEC_NAME     "/arbiter_decisions"       /* /dev/shmem/arbiter_decisions */
#define ARB_DEC_MAGIC    0x53434544u                /* "DECS" */
#define ARB_DEC_VERSION  2u                         /* 2: seqlock slots */
#define ARB_DEC_SLOTS    1024u                      /* at the contract's 50 commands/s: 20 s */

typedef struct {
    uint64_t      index;            /* record sequence number (= head when written) */
    int64_t       t_ns;             /* guardian's CLOCK_MONOTONIC when the decision started */
    uint64_t      t_cycles;         /* ClockCycles() when the command was received */
    uint64_t      latency_cycles;   /* ClockCycles() from receipt to the verdict being ready (NFR-Q4) */
    arb_cmd_t     cmd;              /* the command as received (32 bytes) */
    arb_verdict_t verdict;          /* rule, verdict, state before and after, forwarded setpoint */
    uint32_t      reserved;         /* 0 */
} arb_dec_record_t;

#define ARB_DEC_WORDS (sizeof(arb_dec_record_t) / sizeof(uint64_t))

/* One slot: its sequence word and the record as atomic 64-bit words. */
typedef struct {
    _Atomic uint64_t seq;
    _Atomic uint64_t w[ARB_DEC_WORDS];
} arb_dec_slot_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t cycles_per_sec;
    _Atomic uint64_t head;          /* number of records ever written */
    arb_dec_slot_t r[ARB_DEC_SLOTS];
} arb_dec_ring_t;

_Static_assert(sizeof(arb_verdict_t) == 28, "arb_verdict_t layout");
_Static_assert(sizeof(arb_dec_record_t) == 96, "arb_dec_record_t layout");
_Static_assert(sizeof(arb_dec_record_t) % sizeof(uint64_t) == 0 && ARB_DEC_WORDS <= ARB_SEQ_MAX_WORDS,
               "a record is whole 64-bit words");
_Static_assert((ARB_DEC_SLOTS & (ARB_DEC_SLOTS - 1u)) == 0, "slots: a power of two");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "64-bit atomics must be lock-free for shared memory");

/* The ring protocol, in one place for the writer, the readers and the host test.
 * Writer, the guardian only: record number head goes into slot
 * head % ARB_DEC_SLOTS, then head + 1 is published (release). */
static inline void arb_dec_put(arb_dec_ring_t *d, const arb_dec_record_t *r)
{
    const uint64_t h = atomic_load_explicit(&d->head, memory_order_relaxed);
    arb_dec_slot_t *e = &d->r[h % ARB_DEC_SLOTS];
    arb_dec_record_t c = *r;

    c.index = h;
    arb_seq_write(&e->seq, e->w, &c, ARB_DEC_WORDS);
    atomic_store_explicit(&d->head, h + 1u, memory_order_release);
}

/* Readers: the number of records ever written. */
static inline uint64_t arb_dec_head(const arb_dec_ring_t *d)
{
    return atomic_load_explicit(&((arb_dec_ring_t *)d)->head, memory_order_acquire);
}

/* Readers: record number i into *out. 1 if the copy is whole and is record i,
 * else 0 (not written yet, being written, or overwritten: drop it and count it,
 * FR-E5; *out is then zero). */
static inline int arb_dec_get(const arb_dec_ring_t *d, uint64_t i, arb_dec_record_t *out)
{
    const arb_dec_slot_t *e = &d->r[i % ARB_DEC_SLOTS];
    uint64_t h2;

    memset(out, 0, sizeof *out);
    if (!arb_seq_read(&e->seq, e->w, out, ARB_DEC_WORDS)) {
        return 0;
    }
    h2 = atomic_load_explicit(&((arb_dec_ring_t *)d)->head, memory_order_acquire);
    return out->index == i && i < h2 && i + ARB_DEC_SLOTS > h2;
}

#endif /* ARB_DECISIONS_H */
