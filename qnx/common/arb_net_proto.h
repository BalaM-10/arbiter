/*
 * Planner protocol, version 1: the 64-byte frames between a planner (or an
 * observer) and the gateway arb-gateway. Specification: docs/PROTOCOL.md; this
 * header must match it field for field (the offsets are checked below, and the
 * documented example frames by qnx/tests/test_gateway.c and
 * host/tests/test_protocol.py).
 *
 * Every integer is little-endian on the wire. Both QNX targets (x86_64 and
 * aarch64le) are little-endian, so the structures are sent as they are; the
 * check below stops a build for any other byte order.
 */
#ifndef ARB_NET_PROTO_H
#define ARB_NET_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "arb_contract_table.h"

#define ARB_NET_FRAME_LEN      64u
#define ARB_NET_VERSION        1u
#define ARB_NET_MAGIC          { 0x41, 0x52, 0x42, 0x50 }   /* "ARBP" */
#define ARB_NET_PLANNER_PORT   7700
#define ARB_NET_OBSERVER_PORT  7701
#define ARB_NET_OBSERVERS_MAX  2
#define ARB_NET_TELEM_PERIOD_MS 10u
#define ARB_NET_QUEUE_FRAMES   64u                         /* send queue per connection */

/* Planner to gateway. */
#define ARB_NET_HELLO     0x01u
#define ARB_NET_COMMAND   0x02u
#define ARB_NET_ESTOP     0x03u
/* Gateway to client. */
#define ARB_NET_WELCOME   0x81u
#define ARB_NET_VERDICT   0x82u
#define ARB_NET_TELEMETRY 0x83u
#define ARB_NET_STATE     0x84u
#define ARB_NET_ERROR     0x8Fu

/* VERDICT result. */
#define ARB_NET_DECIDED      0u
#define ARB_NET_REFUSED      1u   /* the guardian refused the submit itself (EPERM, EBUSY) */
#define ARB_NET_UNAVAILABLE  2u   /* the guardian could not be reached */
#define ARB_NET_REJECTED     3u   /* the guardian rejected the message as malformed (EBADMSG) */

/* ERROR codes. */
#define ARB_NET_E_MAGIC       1u
#define ARB_NET_E_VERSION     2u
#define ARB_NET_E_TYPE        3u
#define ARB_NET_E_RESERVED    4u
#define ARB_NET_E_ORDER       5u
#define ARB_NET_E_BUSY        6u
#define ARB_NET_E_UNAVAILABLE 7u
#define ARB_NET_E_SLOW        8u
#define ARB_NET_E_READ_ONLY   9u

typedef struct {
    uint8_t  magic[4];
    uint8_t  version;
    uint8_t  type;
    uint16_t reserved;
} arb_net_hdr_t;

typedef struct {
    arb_net_hdr_t h;
    uint32_t seq;
    uint32_t duration_ms;
    uint64_t sender_ts_ns;
    int32_t  target_urad[ARB_JOINTS];
    uint8_t  zero[32];
} arb_net_command_t;

typedef struct {
    arb_net_hdr_t h;
    uint8_t  state;
    uint8_t  stop_cause;
    uint8_t  has_seq;
    uint8_t  joints;
    uint32_t last_seq;
    uint32_t session;
    uint32_t telemetry_period_ms;
    uint8_t  zero[40];
} arb_net_welcome_t;

typedef struct {
    arb_net_hdr_t h;
    uint32_t seq;
    uint8_t  result;
    uint8_t  verdict;
    uint8_t  rule;
    uint8_t  state;
    uint8_t  flags;
    uint8_t  reserved0;
    uint16_t reserved1;
    int32_t  fwd_urad[ARB_JOINTS];
    uint32_t fwd_duration_ms;
    uint32_t requested_ms;
    uint8_t  zero[28];
} arb_net_verdict_t;

typedef struct {
    arb_net_hdr_t h;
    uint64_t index;
    int64_t  t_ns;
    int32_t  pos_urad[ARB_JOINTS];
    int32_t  vel_urad_s[ARB_JOINTS];
    uint32_t segment;
    uint8_t  arm_state;
    uint8_t  lease_ok;
    uint16_t reserved;
    uint32_t dropped;
    uint8_t  zero[12];
} arb_net_telemetry_t;

typedef struct {
    arb_net_hdr_t h;
    uint8_t  state;
    uint8_t  stop_cause;
    uint8_t  has_seq;
    uint8_t  reserved;
    uint32_t last_seq;
    uint32_t decisions;
    uint16_t estops;
    uint16_t watchdog_lost;
    uint16_t submits_refused;
    uint16_t resets;
    uint8_t  zero[36];
} arb_net_state_t;

typedef struct {
    arb_net_hdr_t h;
    uint8_t  code;
    uint8_t  zero[55];
} arb_net_error_t;

typedef union {
    uint8_t             b[ARB_NET_FRAME_LEN];
    arb_net_hdr_t       h;
    arb_net_command_t   command;
    arb_net_welcome_t   welcome;
    arb_net_verdict_t   verdict;
    arb_net_telemetry_t telemetry;
    arb_net_state_t     state;
    arb_net_error_t     error;
} arb_net_frame_t;

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the planner protocol is little-endian; this target is not"
#endif

/* Every frame is 64 bytes, and every field sits where docs/PROTOCOL.md says. */
_Static_assert(sizeof(arb_net_hdr_t) == 8, "header");
_Static_assert(sizeof(arb_net_command_t) == 64 && sizeof(arb_net_welcome_t) == 64 &&
               sizeof(arb_net_verdict_t) == 64 && sizeof(arb_net_telemetry_t) == 64 &&
               sizeof(arb_net_state_t) == 64 && sizeof(arb_net_error_t) == 64 &&
               sizeof(arb_net_frame_t) == 64, "every frame is 64 bytes");
_Static_assert(offsetof(arb_net_command_t, seq) == 8 && offsetof(arb_net_command_t, duration_ms) == 12 &&
               offsetof(arb_net_command_t, sender_ts_ns) == 16 && offsetof(arb_net_command_t, target_urad) == 24,
               "COMMAND");
_Static_assert(offsetof(arb_net_welcome_t, last_seq) == 12 && offsetof(arb_net_welcome_t, session) == 16 &&
               offsetof(arb_net_welcome_t, telemetry_period_ms) == 20, "WELCOME");
_Static_assert(offsetof(arb_net_verdict_t, result) == 12 && offsetof(arb_net_verdict_t, flags) == 16 &&
               offsetof(arb_net_verdict_t, fwd_urad) == 20 && offsetof(arb_net_verdict_t, fwd_duration_ms) == 28 &&
               offsetof(arb_net_verdict_t, requested_ms) == 32, "VERDICT");
_Static_assert(offsetof(arb_net_telemetry_t, index) == 8 && offsetof(arb_net_telemetry_t, t_ns) == 16 &&
               offsetof(arb_net_telemetry_t, pos_urad) == 24 && offsetof(arb_net_telemetry_t, vel_urad_s) == 32 &&
               offsetof(arb_net_telemetry_t, segment) == 40 && offsetof(arb_net_telemetry_t, arm_state) == 44 &&
               offsetof(arb_net_telemetry_t, dropped) == 48, "TELEMETRY");
_Static_assert(offsetof(arb_net_state_t, last_seq) == 12 && offsetof(arb_net_state_t, decisions) == 16 &&
               offsetof(arb_net_state_t, estops) == 20 && offsetof(arb_net_state_t, resets) == 26, "STATE");
_Static_assert(ARB_JOINTS == 2, "the protocol carries two joints");

#endif /* ARB_NET_PROTO_H */
