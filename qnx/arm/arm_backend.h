/*
 * The arm controller's hardware boundary (FR-A5): the control loop talks only to
 * this interface, so a servo backend can replace the simulation without touching
 * the loop. M1 has one backend, the simulation (arm_backend_sim.c).
 */
#ifndef ARM_BACKEND_H
#define ARM_BACKEND_H

#include <stdint.h>

#include "arb_contract_table.h"

typedef struct arm_backend arm_backend_t;

struct arm_backend {
    const char *name;
    /* Called once at startup with the initial position. Returns 0 on success. */
    int  (*init)(arm_backend_t *b, const int32_t pos_urad[ARB_JOINTS]);
    /* Called every control cycle with the commanded position and velocity. */
    void (*apply)(arm_backend_t *b, const int32_t pos_urad[ARB_JOINTS], const int32_t vel_urad_s[ARB_JOINTS]);
    /* The backend's measured position (the simulation reports what it was given). */
    void (*read)(const arm_backend_t *b, int32_t pos_urad[ARB_JOINTS]);
    int32_t state_pos[ARB_JOINTS];
};

/* The simulated arm: an ideal actuator that follows the commanded position. */
void arm_backend_sim_bind(arm_backend_t *b);

#endif /* ARM_BACKEND_H */
