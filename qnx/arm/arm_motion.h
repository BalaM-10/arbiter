/*
 * Simulated motion of the 2-joint arm (FR-A2, FR-A3). Pure: no OS call, no clock
 * read; time is passed in. Shared by the arm controller and its host tests.
 *
 * The arm executes one straight joint-space segment at a time, from a start
 * position to a target over a duration, both joints arriving together.
 * Motion is a function of time, not of how many control ticks arrive: extra or
 * missing ticks change how often the arm looks, never how fast it moves.
 *
 * Stopping decelerates along the current segment at ARB_BRAKE_DECEL_URAD_S2
 * (fastest joint), so a stop never leaves the path the guardian checked.
 */
#ifndef ARM_MOTION_H
#define ARM_MOTION_H

#include <stdint.h>

#include "arb_contract_table.h"

typedef enum {
    ARM_STATE_IDLE     = 1,   /* holding position, ready for a setpoint */
    ARM_STATE_MOVING   = 2,
    ARM_STATE_STOPPING = 3,   /* decelerating along the segment (hold or brake) */
    ARM_STATE_BRAKED   = 4    /* stopped and latched: only a new setpoint moves it */
} arm_state_t;

typedef struct {
    int32_t start_urad[ARB_JOINTS];
    int32_t target_urad[ARB_JOINTS];
    int64_t t0_ns;            /* segment start */
    int64_t dur_ns;           /* segment duration (> 0) */
    int     stopping;         /* a stop was requested at tb_ns */
    int     latched;          /* the stop is a latched brake */
    int64_t tb_ns;            /* when the stop was requested */
    double  sb;               /* path fraction at tb */
    double  vb;               /* path speed at tb, per ns */
    double  ab;               /* path deceleration, per ns^2 */
} arm_motion_t;

typedef struct {
    int32_t pos_urad[ARB_JOINTS];
    int32_t vel_urad_s[ARB_JOINTS];
    uint8_t state;            /* arm_state_t */
} arm_sample_t;

/* Start at rest at pos. */
void arm_motion_init(arm_motion_t *m, const int32_t pos_urad[ARB_JOINTS], int64_t now_ns);

/* Replace the active segment: from `from` (the arm's actual position now) to
 * `to` over duration_ms (>= 1). Clears a latched brake. */
void arm_motion_start(arm_motion_t *m, const int32_t from_urad[ARB_JOINTS],
                      const int32_t to_urad[ARB_JOINTS], uint32_t duration_ms, int64_t now_ns);

/* Decelerate to a stop along the current segment. latch = 1 for a brake that
 * stays engaged until the next setpoint; 0 for a hold. A second call can only
 * upgrade a hold to a latched brake. */
void arm_motion_stop(arm_motion_t *m, int64_t now_ns, int latch);

/* Position, velocity and state at now_ns (now_ns >= the last start/stop time). */
void arm_motion_sample(const arm_motion_t *m, int64_t now_ns, arm_sample_t *out);

/* The shortest duration (ms) that keeps every joint at or under vmax for this
 * move (the arm's own check, independent of the guardian; FR-A3). */
uint32_t arm_motion_min_duration_ms(const int32_t from_urad[ARB_JOINTS],
                                    const int32_t to_urad[ARB_JOINTS], uint32_t vmax_urad_s);

#endif /* ARM_MOTION_H */
