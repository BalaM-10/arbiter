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

#endif /* ARB_ARM_MODEL_H */
