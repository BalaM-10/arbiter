/*
 * Geometry of the simulated 2-joint planar arm (FR-A2). A property of the arm,
 * not of the safety contract, so it is a shared constant. The host bench
 * mirrors these values for its independent kinematics.
 *
 * Base at the origin. Joint 1 rotates link 1 from the +x axis; joint 2 rotates
 * link 2 relative to link 1. Angles in microradians, lengths in micrometres.
 *   elbow = L1 * (cos q1, sin q1)
 *   tip   = elbow + L2 * (cos(q1 + q2), sin(q1 + q2))
 */
#ifndef ARB_ARM_MODEL_H
#define ARB_ARM_MODEL_H

#define ARB_LINK1_UM 300000   /* 0.30 m */
#define ARB_LINK2_UM 250000   /* 0.25 m */

/* Control loop period (FR-A1, NFR-T1 target). */
#define ARB_ARM_PERIOD_NS 1000000L

/* Motion lease: an authorization expires after this long without renewal
 * (docs/DESIGN_M1_ADDENDUM.md section 2; FR-A4's "configured number of cycles"). */
#define ARB_LEASE_MS 10

/* A setpoint's start may differ from the arm's actual position by at most the
 * distance covered at vmax in this many control cycles (addendum section 5).
 * libcontract adds the matching workspace margin to every keep-out check. */
#define ARB_START_TOL_MS 5

/* Simulated brake: deceleration of the fastest joint along the current segment. */
#define ARB_BRAKE_DECEL_URAD_S2 10000000   /* 10 rad/s^2 */

#endif /* ARB_ARM_MODEL_H */
