/* Simulated backend (FR-A2, FR-A5): follows the commanded position exactly. */
#include <string.h>

#include "arm_backend.h"

static int sim_init(arm_backend_t *b, const int32_t pos_urad[ARB_JOINTS])
{
    memcpy(b->state_pos, pos_urad, sizeof b->state_pos);
    return 0;
}

static void sim_apply(arm_backend_t *b, const int32_t pos_urad[ARB_JOINTS], const int32_t vel_urad_s[ARB_JOINTS])
{
    (void)vel_urad_s;
    memcpy(b->state_pos, pos_urad, sizeof b->state_pos);
}

static void sim_read(const arm_backend_t *b, int32_t pos_urad[ARB_JOINTS])
{
    memcpy(pos_urad, b->state_pos, sizeof b->state_pos);
}

void arm_backend_sim_bind(arm_backend_t *b)
{
    memset(b, 0, sizeof *b);
    b->name = "sim";
    b->init = sim_init;
    b->apply = sim_apply;
    b->read = sim_read;
}
