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

/* Requested duration of a move (FR-G2). */
#define ARB_DURATION_MIN_MS  1u
#define ARB_DURATION_MAX_MS  60000u

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

#endif /* ARB_PROTO_H */
