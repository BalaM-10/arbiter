/*
 * Binary safety-contract table: the only form of the contract the guardian
 * ever sees (FR-C3, FR-C5). Written by the host compiler (`arbiter contract
 * compile`), read by libcontract's arb_contract_load() (FR-C4).
 *
 * Layout rules
 * - Little-endian, fixed offsets, no implicit padding. Both targets (x86_64,
 *   aarch64le) are little-endian; the host compiler packs with '<'.
 * - The Python compiler mirrors this layout; the cross-language test
 *   (host/tests, qnx/tests) fails if the two drift apart.
 * - crc32 is CRC-32/IEEE (as zlib.crc32) over every byte before the crc32 field.
 *
 * Units: angles in microradians (urad), lengths in micrometres (um),
 * velocities in urad/s, times in milliseconds. Integers only, so a contract can
 * never carry NaN or infinity.
 */
#ifndef ARB_CONTRACT_TABLE_H
#define ARB_CONTRACT_TABLE_H

#include <stddef.h>
#include <stdint.h>

#define ARB_CONTRACT_MAGIC    0x43425241u /* bytes "ARBC" in little-endian order */
#define ARB_CONTRACT_VERSION  1u
#define ARB_JOINTS            2
#define ARB_MAX_ZONES         4

/* Ranges every loaded contract must satisfy (checked by the compiler and again
 * by arb_contract_load, which fails closed). */
#define ARB_ANGLE_LIMIT_URAD        3141592      /* |joint limit| <= pi */
#define ARB_VMAX_MIN_URAD_S         1000u        /* 0.001 rad/s */
#define ARB_VMAX_MAX_URAD_S         10000000u    /* 10 rad/s */
#define ARB_SILENCE_MIN_MS          10u
#define ARB_SILENCE_MAX_MS          60000u
#define ARB_RATE_MIN_PER_S          1u
#define ARB_RATE_MAX_PER_S          10000u
#define ARB_BURST_MIN               1u
#define ARB_BURST_MAX               1000u
#define ARB_MAX_VIOLATION_THRESHOLD 16u
#define ARB_WINDOW_MIN_MS           1u
#define ARB_WINDOW_MAX_MS           600000u
#define ARB_WORKSPACE_LIMIT_UM      2000000      /* zone corners within +-2 m */
#define ARB_DURATION_MIN_MS         1u           /* shortest requested move */
#define ARB_DURATION_CEIL_MS        60000u       /* largest allowed contract max_duration_ms */

typedef struct {
    int32_t x_min_um;
    int32_t y_min_um;
    int32_t x_max_um;
    int32_t y_max_um;
} arb_zone_t;

typedef struct {
    uint32_t   magic;                      /* ARB_CONTRACT_MAGIC */
    uint16_t   version;                    /* ARB_CONTRACT_VERSION */
    uint16_t   table_size;                 /* sizeof(arb_contract_table_t) */
    uint8_t    source_sha256[32];          /* SHA-256 of the YAML source (FR-C3, FR-C6) */
    int32_t    joint_min_urad[ARB_JOINTS];
    int32_t    joint_max_urad[ARB_JOINTS];
    uint32_t   vmax_urad_s;                /* maximum speed of any joint */
    uint32_t   silence_timeout_ms;         /* AI silence before HOLD (NFR-T4) */
    uint32_t   rate_per_s;                 /* sustained command rate */
    uint32_t   rate_burst;                 /* token-bucket depth */
    uint32_t   violation_threshold;        /* N counted denials ... */
    uint32_t   violation_window_ms;        /* ... within W ms -> SAFE_STOP */
    uint32_t   max_duration_ms;            /* longest requested move; >= a full-range move at vmax */
    uint32_t   zone_count;                 /* 0..ARB_MAX_ZONES */
    arb_zone_t zones[ARB_MAX_ZONES];       /* keep-out rectangles, workspace frame */
    uint32_t   crc32;                      /* over bytes [0, offsetof(crc32)) */
} arb_contract_table_t;

_Static_assert(offsetof(arb_contract_table_t, version) == 4, "layout");
_Static_assert(offsetof(arb_contract_table_t, source_sha256) == 8, "layout");
_Static_assert(offsetof(arb_contract_table_t, joint_min_urad) == 40, "layout");
_Static_assert(offsetof(arb_contract_table_t, vmax_urad_s) == 56, "layout");
_Static_assert(offsetof(arb_contract_table_t, max_duration_ms) == 80, "layout");
_Static_assert(offsetof(arb_contract_table_t, zone_count) == 84, "layout");
_Static_assert(offsetof(arb_contract_table_t, zones) == 88, "layout");
_Static_assert(offsetof(arb_contract_table_t, crc32) == 152, "layout");
_Static_assert(sizeof(arb_contract_table_t) == 156, "layout");

#endif /* ARB_CONTRACT_TABLE_H */
