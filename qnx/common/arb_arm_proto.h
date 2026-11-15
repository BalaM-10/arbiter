/*
 * Internal guardian <-> arm controller messages. Not reachable through the
 * gateway: the arm registers an internal name with name_attach() for the guardian
 * (docs/DESIGN_M1_ADDENDUM.md, decision 7), and accepts setpoints only from the
 * guardian (FR-G6). Message types are above _IO_MAX, the range reserved for
 * system messages:
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html
 */
#ifndef ARB_ARM_PROTO_H
#define ARB_ARM_PROTO_H

#include <stdint.h>
#include <sys/iomsg.h>

#include "arb_contract_table.h"

#define ARB_ARM_NAME          "arbiter_arm"   /* appears as /dev/name/local/arbiter_arm */
#define ARB_ARM_PROTO_VERSION 1u

enum {
    ARB_ARM_MSG_GET_STATE = _IO_MAX + 1,
    ARB_ARM_MSG_SETPOINT  = _IO_MAX + 2,
    ARB_ARM_MSG_RESET     = _IO_MAX + 3    /* FR-G5: operator reset, from the bound guardian only */
};

typedef struct {
    uint16_t type;                     /* ARB_ARM_MSG_* (first, as QNX message headers are) */
    uint16_t version;                  /* ARB_ARM_PROTO_VERSION */
    uint32_t seq;                      /* echoed in the reply */
} arb_arm_hdr_t;

typedef struct {
    arb_arm_hdr_t hdr;
} arb_arm_get_state_t;

typedef struct {
    arb_arm_hdr_t hdr;
} arb_arm_reset_t;

typedef struct {
    arb_arm_hdr_t hdr;
    int32_t  start_urad[ARB_JOINTS];   /* the position the guardian checked from */
    int32_t  target_urad[ARB_JOINTS];
    uint32_t duration_ms;
    uint32_t reserved;
} arb_arm_setpoint_t;

enum {
    ARB_ARM_OK          = 0,
    ARB_ARM_STALE_START = 1,           /* the arm had moved too far from start_urad */
    ARB_ARM_REJECTED    = 2,           /* refused (sender, size, version, ...) */
    ARB_ARM_NO_WATCHDOG = 3            /* reset refused: the arm could not register with a watchdog */
};

typedef struct {
    uint16_t version;
    uint16_t status;                   /* ARB_ARM_* */
    uint32_t seq;                      /* from the request */
    int32_t  pos_urad[ARB_JOINTS];
    int32_t  vel_urad_s[ARB_JOINTS];
    uint32_t arm_state;
    uint32_t reserved;
} arb_arm_reply_t;

_Static_assert(sizeof(arb_arm_get_state_t) == 8, "layout");
_Static_assert(sizeof(arb_arm_reset_t) == 8, "layout");
_Static_assert(sizeof(arb_arm_setpoint_t) == 32, "layout");
_Static_assert(sizeof(arb_arm_reply_t) == 32, "layout");

#endif /* ARB_ARM_PROTO_H */
