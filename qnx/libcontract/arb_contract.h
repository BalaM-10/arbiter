/*
 * libcontract: the guardian's decision logic as a pure library (NFR-K2).
 *
 * Pure means: no OS call, no clock read, no allocation, no I/O, no global
 * mutable state. Time is passed in by the caller. The same code is unit-tested
 * on the host and on QNX. Uses only <stdint.h>, <stddef.h>, <string.h> and
 * <math.h> (sin, cos, ceil) from the C standard library.
 *
 * Decision order for every command (FR-G4):
 *   1 well-formed (incl. duration bounds)  2 state gate  3 sequence
 *   4 rate limit  5 joint limits  6 velocity (CLAMP)
 *   7 keep-out zones along the whole path
 * The first failing check decides. When unsure, deny.
 */
#ifndef ARB_CONTRACT_H
#define ARB_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#include "arb_contract_table.h"
#include "arb_proto.h"

/* Verdicts (FR-G3). 0 is never a valid verdict, so an unset result is visible. */
typedef enum {
    ARB_VERDICT_INVALID = 0,
    ARB_VERDICT_ALLOW   = 1,
    ARB_VERDICT_CLAMP   = 2,
    ARB_VERDICT_DENY    = 3
} arb_verdict_kind_t;

/* Rule that decided (FR-G3). */
typedef enum {
    ARB_RULE_NONE        = 0,  /* ALLOW: no rule objected */
    ARB_RULE_MALFORMED   = 1,
    ARB_RULE_SEQUENCE    = 2,
    ARB_RULE_RATE        = 3,
    ARB_RULE_JOINT_LIMIT = 4,
    ARB_RULE_VELOCITY    = 5,  /* CLAMP: duration stretched */
    ARB_RULE_KEEPOUT     = 6,
    ARB_RULE_STATE       = 7,  /* DENY: guardian is in SAFE_STOP */
    ARB_RULE_INTERNAL    = 8,  /* DENY: caller passed an impossible arm state or contract */
    ARB_RULE_DURATION    = 9,  /* DENY: requested duration 0, above the contract max, or
                                  a CLAMP that would need more than the max */
    ARB_RULE_ARM_FAULT   = 10, /* DENY: approved, but the arm did not answer in time (FR-G9) */
    ARB_RULE_STALE_START = 11  /* DENY: approved, but the arm had moved from the checked start */
} arb_rule_t;

/* Verdict flags. */
#define ARB_VFLAG_KEEPOUT_EXIT 0x01u /* move starts inside a keep-out zone and only exits it */

/* Guardian states (FR-G5). Nonzero so an uninitialized guard is visible. */
typedef enum {
    ARB_STATE_HOLD      = 1,
    ARB_STATE_NORMAL    = 2,
    ARB_STATE_SAFE_STOP = 3
} arb_state_t;

/* Why SAFE_STOP was entered. */
typedef enum {
    ARB_STOP_NONE       = 0,
    ARB_STOP_VIOLATIONS = 1,   /* threshold of counted denials reached */
    ARB_STOP_ARM_TIMEOUT= 2,   /* setpoint send to the arm timed out (FR-G9) */
    ARB_STOP_WATCHDOG   = 3,
    ARB_STOP_ESTOP      = 4
} arb_stop_cause_t;

/* Contract table load results (FR-C4). */
typedef enum {
    ARB_LOAD_OK          = 0,
    ARB_LOAD_E_ARG       = 1,  /* NULL pointer */
    ARB_LOAD_E_SIZE      = 2,  /* buffer length != table size */
    ARB_LOAD_E_MAGIC     = 3,
    ARB_LOAD_E_VERSION   = 4,
    ARB_LOAD_E_TABLESIZE = 5,  /* table_size field wrong */
    ARB_LOAD_E_CRC       = 6,
    ARB_LOAD_E_RANGE     = 7   /* a value outside the allowed ranges */
} arb_load_result_t;

/* Path sampling for the keep-out check: samples 0..ARB_PATH_SAMPLES inclusive. */
#define ARB_PATH_SAMPLES 128

/* Result of one decision. */
typedef struct {
    uint8_t  verdict;                  /* arb_verdict_kind_t */
    uint8_t  rule;                     /* arb_rule_t */
    uint8_t  state_before;             /* arb_state_t */
    uint8_t  state_after;              /* arb_state_t */
    uint8_t  flags;                    /* ARB_VFLAG_* */
    uint8_t  reserved_[3];
    uint32_t seq;                      /* echoed when the command was well-formed, else 0 */
    int32_t  target_urad[ARB_JOINTS];  /* setpoint to forward (ALLOW/CLAMP), else 0 */
    uint32_t requested_ms;             /* as sent (well-formed commands), else 0 */
    uint32_t duration_ms;              /* to forward: requested, or stretched on CLAMP */
} arb_verdict_t;

/* Mutable decision state. One per guardian, owned by its single thread. */
typedef struct {
    const arb_contract_table_t *contract;
    uint8_t  state;                    /* arb_state_t */
    uint8_t  stop_cause;               /* arb_stop_cause_t */
    uint8_t  have_seq;
    uint32_t last_seq;
    uint64_t tokens_milli;             /* rate bucket, 1000 per command */
    uint64_t refill_ns;                /* time up to which tokens were credited */
    uint64_t last_accept_ns;           /* last ALLOW or CLAMP */
    uint64_t violation_ns[ARB_MAX_VIOLATION_THRESHOLD]; /* recent counted denials */
    uint32_t violation_next;           /* ring index */
    uint32_t violation_fill;           /* entries in use */
} arb_guard_t;

/* CRC-32/IEEE, identical to zlib.crc32. */
uint32_t arb_crc32(const void *buf, size_t len);

/* Validate a table and copy it to *out (FR-C4). *out is written only on ARB_LOAD_OK. */
arb_load_result_t arb_contract_load(const void *buf, size_t len, arb_contract_table_t *out);

/* Start a guard on a contract that arb_contract_load() accepted. Starts in HOLD. */
void arb_guard_init(arb_guard_t *g, const arb_contract_table_t *contract, uint64_t now_ns);

/* Decide one command in one call (FR-G3, FR-G4, FR-G5): the three steps below
 * run back to back, as if the arm accepted the setpoint. msg/len are untrusted
 * bytes; current_urad is the arm's position, the start of the path. */
void arb_guard_check(arb_guard_t *g, const void *msg, size_t len,
                     const int32_t current_urad[ARB_JOINTS], uint64_t now_ns,
                     arb_verdict_t *out);

/* The same decision in three steps, as the guardian runs it:
 *
 * 1. arb_guard_check_pre(): checks 1 to 5 (well-formed, state, sequence, rate,
 *    joint limits), which need no arm position. Returns 1 and fills *cmd if the
 *    command passed them; otherwise returns 0 with the final DENY in *out. A
 *    flood of invalid commands therefore never causes a query to the arm.
 * 2. arb_guard_check_motion(): checks 6 and 7 (velocity, keep-out along the path)
 *    from the arm's current position. Records keep-out violations, but does not
 *    yet count the command as accepted.
 * 3. arb_guard_commit_accept(): call once the arm has accepted the setpoint;
 *    this is what makes the AI "not silent" and moves HOLD to NORMAL. If the arm
 *    refuses or times out, call arb_guard_reject_after_approval() instead. */
int  arb_guard_check_pre(arb_guard_t *g, const void *msg, size_t len, uint64_t now_ns,
                         arb_cmd_t *cmd, arb_verdict_t *out);
void arb_guard_check_motion(arb_guard_t *g, const arb_cmd_t *cmd,
                            const int32_t current_urad[ARB_JOINTS], uint64_t now_ns,
                            arb_verdict_t *out);
void arb_guard_commit_accept(arb_guard_t *g, uint64_t now_ns, arb_verdict_t *out);
void arb_guard_reject_after_approval(arb_guard_t *g, arb_rule_t rule, arb_verdict_t *out);

/* The last client disconnected or died (resource manager close handler):
 * NORMAL -> HOLD at once, without waiting for the silence timeout (TM-C4).
 * Returns 1 on a change. */
int arb_guard_client_lost(arb_guard_t *g);

/* Periodic tick: NORMAL -> HOLD after the silence timeout. Returns 1 on a change. */
int arb_guard_tick(arb_guard_t *g, uint64_t now_ns);

/* Force SAFE_STOP for an external fault (arm timeout, watchdog, e-stop). */
void arb_guard_fault(arb_guard_t *g, arb_stop_cause_t cause);

/* Operator reset: SAFE_STOP -> HOLD. Returns 0, or -1 if not in SAFE_STOP. */
int arb_guard_reset(arb_guard_t *g, uint64_t now_ns);

/* Forward kinematics (FR-G4 keep-out, shared with the bench's cross-check). */
void arb_fk(const double q_rad[ARB_JOINTS], double elbow_um[2], double tip_um[2]);

#endif /* ARB_CONTRACT_H */
