/*
 * Arm telemetry (FR-A6): a ring of per-cycle samples in shared memory, written
 * only by the arm controller and mapped read-only by readers (tests, bench, later
 * the gateway). Single writer, lock-free: readers never block the arm.
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
#define ARB_TELEM_VERSION 1u
#define ARB_TELEM_SLOTS   8192u                      /* 8 s at 1 kHz */

typedef struct {
    uint64_t t_cycles;                 /* ClockCycles() at the sample */
    uint64_t index;                    /* slot sequence number (= head when written) */
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
    arb_telem_sample_t s[ARB_TELEM_SLOTS];
} arb_telem_t;

_Static_assert(sizeof(arb_telem_sample_t) == 40, "layout");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "64-bit atomics must be lock-free for shared memory");

#endif /* ARB_TELEMETRY_H */
