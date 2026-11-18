/*
 * Wire structures shared by the gateway, guardian and test clients.
 * Fixed size, version field first, validated before any field is used.
 * Integers only: untrusted input can never be NaN or infinity.
 */
#ifndef ARB_PROTO_H
#define ARB_PROTO_H

#include <stdint.h>

#include "arb_contract_table.h"

#define ARB_PROTO_VERSION    1u

/* The requested duration of a move must lie in [ARB_DURATION_MIN_MS, the
 * contract's max_duration_ms] (FR-G2); see arb_contract_table.h. */

/* A motion command from the planner (FR-G2). 32 bytes. */
typedef struct {
    uint16_t version;                  /* ARB_PROTO_VERSION */
    uint16_t flags;                    /* must be 0 */
    uint32_t seq;                      /* strictly newer than the last accepted (serial arithmetic) */
    uint64_t sender_ts_ns;             /* sender's clock; logged, never trusted for decisions */
    int32_t  target_urad[ARB_JOINTS];  /* joint targets */
    uint32_t duration_ms;              /* requested time to reach the targets */
    uint32_t reserved;                 /* must be 0 */
} arb_cmd_t;

_Static_assert(sizeof(arb_cmd_t) == 32, "arb_cmd_t layout");

/* The guardian's answer to one command (FR-G3). 32 bytes. */
typedef struct {
    uint16_t version;                  /* ARB_PROTO_VERSION */
    uint8_t  verdict;                  /* ALLOW=1, CLAMP=2, DENY=3 */
    uint8_t  rule;                     /* rule that decided (arb_rule_t in libcontract) */
    uint8_t  state;                    /* guardian state after the decision */
    uint8_t  flags;                    /* e.g. keep-out exit move */
    uint16_t reserved0;
    uint32_t seq;                      /* echoed */
    int32_t  target_urad[ARB_JOINTS];  /* forwarded setpoint (ALLOW/CLAMP), else 0 */
    uint32_t duration_ms;              /* forwarded duration (stretched on CLAMP), else 0 */
    uint32_t requested_ms;             /* as sent */
    uint32_t reserved1;
} arb_verdict_wire_t;

_Static_assert(sizeof(arb_verdict_wire_t) == 32, "arb_verdict_wire_t layout");

/* Guardian status (for tests, the gateway and the dashboard). 56 bytes. */
typedef struct {
    uint16_t version;
    uint8_t  state;                    /* HOLD=1, NORMAL=2, SAFE_STOP=3 */
    uint8_t  stop_cause;               /* 0 none, 1 violations, 2 arm timeout, 3 watchdog, 4 e-stop */
    uint32_t clients;                  /* open connections to /dev/arbiter */
    uint32_t decisions;                /* commands decided */
    uint32_t arm_faults;               /* arm sends that failed or timed out */
    uint32_t ticks;                    /* 1 ms ticks handled */
    uint32_t bad_messages;             /* devctl messages rejected before decision */
    uint32_t arm_pulse_errors;         /* lease/hold/brake pulses the kernel refused */
    uint32_t watchdog_trips;           /* genuine trip events from the watchdog */
    uint32_t foreign_pulses;           /* tick, trip or beat pulses not from our own connection (or without our token), ignored */
    uint16_t watchdog_lost;            /* times the guardian lost the watchdog (FR-W5, FR-W6) */
    uint16_t watchdog_lost_cause;      /* ARB_WD_LOST_* of the first loss, 0 if none */
    uint16_t estops;                   /* emergency stops received (FR-W4) */
    uint16_t resets;                   /* operator resets done (FR-G5) */
    uint16_t resets_refused;           /* resets refused: not the operator, wrong state, no watchdog, arm */
    uint16_t opens_refused;            /* FR-G9: opens refused: the pool or the user's share full */
    uint32_t last_seq;                 /* FR-G10: newest sequence number consumed; the next
                                          command must be newer (valid when has_seq is 1) */
    uint8_t  has_seq;                  /* 1 once any command passed the sequence check */
    uint8_t  reserved1;                /* 0 */
    uint16_t submits_refused;          /* FR-G10: SUBMITs refused: not the gateway user, or
                                          not the current submitter's open */
} arb_status_t;

_Static_assert(sizeof(arb_status_t) == 56, "arb_status_t layout");

/* devctl() payload for a command: the command in, the verdict out (same buffer). */
typedef union {
    arb_cmd_t          cmd;
    arb_verdict_wire_t verdict;
} arb_submit_t;

_Static_assert(sizeof(arb_submit_t) == 32, "arb_submit_t layout");

/* devctl() command codes for these payloads: qnx/common/arb_devctl.h (QNX only). */

#endif /* ARB_PROTO_H */
