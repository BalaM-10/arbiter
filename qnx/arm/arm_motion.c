/*
 * Simulated arm motion. Pure (see arm_motion.h). Doubles are used only inside,
 * from integers already range-checked by the caller; outputs are integers.
 */
#include <math.h>
#include <string.h>

#include "arb_arm_model.h"
#include "arm_motion.h"

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

static int64_t max_delta(const arm_motion_t *m)
{
    int64_t d = 0;

    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        const int64_t a = abs64((int64_t)m->target_urad[j] - (int64_t)m->start_urad[j]);
        if (a > d) {
            d = a;
        }
    }
    return d;
}

void arm_motion_init(arm_motion_t *m, const int32_t pos_urad[ARB_JOINTS], int64_t now_ns)
{
    memset(m, 0, sizeof *m);
    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        m->start_urad[j] = pos_urad[j];
        m->target_urad[j] = pos_urad[j];
    }
    m->t0_ns = now_ns;
    m->dur_ns = 1;            /* a zero-length segment, already complete */
}

void arm_motion_start(arm_motion_t *m, const int32_t from_urad[ARB_JOINTS],
                      const int32_t to_urad[ARB_JOINTS], uint32_t duration_ms, int64_t now_ns)
{
    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        m->start_urad[j] = from_urad[j];
        m->target_urad[j] = to_urad[j];
    }
    m->t0_ns = now_ns;
    m->dur_ns = (int64_t)(duration_ms < 1u ? 1u : duration_ms) * 1000000;
    m->stopping = 0;
    m->latched = 0;
}

/* Path fraction and path speed (per ns) of the plain segment at time t. */
static void segment_at(const arm_motion_t *m, int64_t t, double *s, double *v)
{
    const double f = (double)(t - m->t0_ns) / (double)m->dur_ns;

    if (f <= 0.0) {
        *s = 0.0;
        *v = 1.0 / (double)m->dur_ns;
    } else if (f >= 1.0) {
        *s = 1.0;
        *v = 0.0;
    } else {
        *s = f;
        *v = 1.0 / (double)m->dur_ns;
    }
}

void arm_motion_stop(arm_motion_t *m, int64_t now_ns, int latch)
{
    const int64_t d = max_delta(m);

    if (m->stopping) {
        if (latch) {
            m->latched = 1;   /* a hold in progress becomes a latched brake */
        }
        return;
    }
    segment_at(m, now_ns, &m->sb, &m->vb);
    m->tb_ns = now_ns;
    m->stopping = 1;
    m->latched = latch ? 1 : 0;
    /* Deceleration of the fastest joint is ARB_BRAKE_DECEL_URAD_S2; in path units
     * that is decel / max|delta| per s^2, i.e. per ns^2 below. */
    m->ab = d > 0 ? ((double)ARB_BRAKE_DECEL_URAD_S2 * 1e-18) / (double)d : 1.0;
}

void arm_motion_sample(const arm_motion_t *m, int64_t now_ns, arm_sample_t *out)
{
    double s, v;
    int moving;

    if (!m->stopping) {
        segment_at(m, now_ns, &s, &v);
        moving = v > 0.0;
        out->state = (uint8_t)(moving ? ARM_STATE_MOVING : ARM_STATE_IDLE);
    } else {
        const double t_stop = m->vb > 0.0 ? m->vb / m->ab : 0.0;   /* ns after tb */
        double dt = (double)(now_ns - m->tb_ns);

        if (dt < 0.0) {
            dt = 0.0;
        }
        if (dt >= t_stop) {
            /* Past the stopping time: exactly stopped. Computing vb - ab * t_stop
             * instead could leave a rounding residue above zero, and the arm would
             * report STOPPING forever instead of BRAKED. */
            s = m->sb + 0.5 * m->vb * t_stop;
            v = 0.0;
        } else {
            s = m->sb + m->vb * dt - 0.5 * m->ab * dt * dt;
            v = m->vb - m->ab * dt;
        }
        if (s >= 1.0) {               /* reached the target while stopping */
            s = 1.0;
            v = 0.0;
        }
        if (v < 0.0) {
            v = 0.0;
        }
        moving = v > 0.0;
        if (moving) {
            out->state = (uint8_t)ARM_STATE_STOPPING;
        } else {
            out->state = (uint8_t)(m->latched ? ARM_STATE_BRAKED : ARM_STATE_IDLE);
        }
    }
    if (s < 0.0) {
        s = 0.0;
    }
    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        const double delta = (double)((int64_t)m->target_urad[j] - (int64_t)m->start_urad[j]);
        out->pos_urad[j] = (int32_t)((int64_t)m->start_urad[j] + (int64_t)llround(delta * s));
        out->vel_urad_s[j] = (int32_t)llround(delta * v * 1e9);
    }
}

uint32_t arm_motion_min_duration_ms(const int32_t from_urad[ARB_JOINTS],
                                    const int32_t to_urad[ARB_JOINTS], uint32_t vmax_urad_s)
{
    uint64_t d = 0;
    uint64_t ms;

    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        const uint64_t a = (uint64_t)abs64((int64_t)to_urad[j] - (int64_t)from_urad[j]);
        if (a > d) {
            d = a;
        }
    }
    if (vmax_urad_s == 0u) {
        return UINT32_MAX;            /* fail closed: no speed allowed, no move possible */
    }
    ms = (d * 1000u + (uint64_t)vmax_urad_s - 1u) / (uint64_t)vmax_urad_s;
    if (ms < 1u) {
        ms = 1u;
    }
    return ms > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}
