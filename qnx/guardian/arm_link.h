/*
 * The guardian's only connection to the arm controller (FR-G6, FR-G9).
 * Every send is bounded with TimerTimeout() (docs/DESIGN_M1_ADDENDUM.md section 3).
 */
#ifndef ARM_LINK_H
#define ARM_LINK_H

#include <stdint.h>

#include "arb_arm_proto.h"

/* Send timeout: 2 ms requested; measured on QEMU as an effective 2 to 4 ms because
 * timers fire on the 1 ms tick (evidence/runs/2026-11-13-qemu/run-133305.log). */
#define ARM_LINK_TIMEOUT_NS 2000000ull

typedef enum {
    ARM_LINK_OK      = 0,
    ARM_LINK_TIMEOUT = 1,   /* the arm did not receive or did not reply in time */
    ARM_LINK_ERROR   = 2,   /* the send failed for another reason, or the reply was malformed */
    ARM_LINK_STALE   = 3    /* the arm rejected the setpoint: it had moved from the checked start */
} arm_link_result_t;

typedef struct {
    int      coid;
    uint32_t seq;
    int      last_errno;    /* errno of the last failed send, for the status log */
} arm_link_t;

/* Connect to the arm's internal name. Returns 0, or -1 with errno set. */
int arm_link_open(arm_link_t *l);

/* Ask the arm where it is. */
arm_link_result_t arm_link_get_state(arm_link_t *l, int32_t pos_urad[ARB_JOINTS]);

/* Send an approved setpoint, starting from the position the guardian checked. */
arm_link_result_t arm_link_send_setpoint(arm_link_t *l, const int32_t start_urad[ARB_JOINTS],
                                         const int32_t target_urad[ARB_JOINTS], uint32_t duration_ms);

#endif /* ARM_LINK_H */
