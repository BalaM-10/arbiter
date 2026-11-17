/*
 * The evidence logger's telemetry relay (decision D1, NFR-I1, FR-N4): the
 * gateway may not map the arm's telemetry ring, so it asks the logger, which
 * maps it read-only, for the arm's newest sample by message passing. The logger
 * only ever replies, at once, so a client can delay nothing but itself.
 *
 * The logger registers ARB_LOGD_NAME with name_attach(); the gateway reaches it
 * with name_open() and MsgSend(). Message types are above _IO_MAX, the range
 * reserved for QNX's own I/O messages, as for the arm (arb_arm_proto.h).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html
 */
#ifndef ARB_LOGD_PROTO_H
#define ARB_LOGD_PROTO_H

#include <stdint.h>
#include <sys/iomsg.h>

#include "arb_telemetry.h"

#define ARB_LOGD_NAME          "arbiter_logd"   /* appears as /dev/name/local/arbiter_logd */
#define ARB_LOGD_PROTO_VERSION 1u
#define ARB_LOGD_MSG_TELEM     (_IO_MAX + 0x21)

typedef struct {
    uint16_t type;                 /* ARB_LOGD_MSG_TELEM */
    uint16_t version;              /* ARB_LOGD_PROTO_VERSION */
    uint32_t reserved;             /* 0 */
} arb_logd_telem_req_t;

typedef struct {
    uint16_t version;              /* ARB_LOGD_PROTO_VERSION */
    uint8_t  have;                 /* 1: s is the arm's newest sample; 0: no arm telemetry (yet) */
    uint8_t  reserved0;
    uint32_t reserved1;
    uint64_t head;                 /* samples the arm has written */
    arb_telem_sample_t s;
} arb_logd_telem_rep_t;

_Static_assert(sizeof(arb_logd_telem_req_t) == 8, "request layout");
_Static_assert(sizeof(arb_logd_telem_rep_t) == 64, "reply layout");

#endif /* ARB_LOGD_PROTO_H */
