/*
 * Arm telemetry (FR-A6): a ring of per-cycle samples in shared memory, written
 * only by the arm controller and mapped read-only by readers: the evidence
 * logger, which relays the newest sample to the gateway by message passing
 * (arb_logd_proto.h, decision D1), and root's test tools and bench. Mode 0640,
 * group ARB_UID_LOGD: the gateway's user cannot open it (NFR-I1). Single writer,
 * lock-free: readers never block the arm.
 *
 * Writer: fill slot head % ARB_TELEM_SLOTS, then publish head + 1 (release).
 * Reader: read head (acquire), copy the slots it wants, read head again; a slot
 * with index < head2 - ARB_TELEM_SLOTS may have been overwritten and is dropped.
 * The 8.0 shared-memory pages state no cross-process ordering rule, so ordering
 * relies on C11 atomics (lock-free 64-bit on x86_64 and aarch64, checked below).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
 */
#ifndef ARB_TELEMETRY_H
#define ARB_TELEMETRY_H

#include <stdatomic.h>
#include <stdint.h>

#include "arb_contract_table.h"

#define ARB_TELEM_NAME    "/arbiter_arm_telemetry"   /* /dev/shmem/arbiter_arm_telemetry */
#define ARB_TELEM_MAGIC   0x4D4C4554u                /* "TELM" */
#define ARB_TELEM_VERSION 3u                      /* 2: t_ns added; 3: wd_lost, wd_lost_cause */
#define ARB_TELEM_SLOTS   8192u                      /* 8 s at 1 kHz */

typedef struct {
    uint64_t t_cycles;                 /* ClockCycles() at the sample */
    uint64_t index;                    /* slot sequence number (= head when written) */
    int64_t  t_ns;                     /* CLOCK_MONOTONIC time the position was computed for:
                                        * the bench derives velocity from pos and t_ns (F6) */
    int32_t  pos_urad[ARB_JOINTS];
    int32_t  vel_urad_s[ARB_JOINTS];
    uint32_t segment;                  /* increments with every accepted setpoint */
    uint8_t  state;                    /* arm_state_t */
    uint8_t  lease_ok;                 /* 1 while the motion lease is valid */
    uint16_t reserved;
} arb_telem_sample_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t cycles_per_sec;
    _Atomic uint64_t head;             /* number of samples ever written */
    _Atomic uint64_t rejected;         /* setpoints/pulses refused (sender, size, limits) */
    _Atomic uint64_t lease_expiries;
    _Atomic uint64_t disconnects;      /* bound guardian connection lost */
    _Atomic int64_t  bound_pid;        /* process the arm accepts commands from, or -1 */
    _Atomic uint64_t wd_brakes;        /* brakes commanded by the watchdog */
    _Atomic uint64_t wd_lost;          /* brakes because the watchdog itself was lost (FR-W5, FR-W6) */
    _Atomic uint64_t wd_lost_cause;    /* ARB_WD_LOST_* of the first such brake, 0 if none */
    arb_telem_sample_t s[ARB_TELEM_SLOTS];
} arb_telem_t;

_Static_assert(sizeof(arb_telem_sample_t) == 48, "layout");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "64-bit atomics must be lock-free for shared memory");

/* The ring protocol, in one place for every writer and reader (and the host test).
 * Writer, the arm only: sample number head goes into slot head % ARB_TELEM_SLOTS,
 * then head + 1 is published (release). */
static inline void arb_telem_put(arb_telem_t *t, const arb_telem_sample_t *s)
{
    const uint64_t h = atomic_load_explicit(&t->head, memory_order_relaxed);
    arb_telem_sample_t *e = &t->s[h % ARB_TELEM_SLOTS];

    *e = *s;
    e->index = h;
    atomic_store_explicit(&t->head, h + 1u, memory_order_release);
}

/* Readers: the number of samples ever written. */
static inline uint64_t arb_telem_head(const arb_telem_t *t)
{
    return atomic_load_explicit(&((arb_telem_t *)t)->head, memory_order_acquire);
}

/* Readers: sample number i into *out. 1 if it is sample i and was not being
 * overwritten while copied, else 0 (not written yet, overwritten, or possibly
 * torn: drop it and count it). */
static inline int arb_telem_get(const arb_telem_t *t, uint64_t i, arb_telem_sample_t *out)
{
    uint64_t h2;

    *out = t->s[i % ARB_TELEM_SLOTS];
    h2 = atomic_load_explicit(&((arb_telem_t *)t)->head, memory_order_acquire);
    return out->index == i && i < h2 && i + ARB_TELEM_SLOTS > h2;
}

#endif /* ARB_TELEMETRY_H */
