/*
 * User IDs of the Arbiter processes (NFR-I5, FR-G6, F11). Each process runs
 * under its own user ID, used also as its group ID. The trusted processes start
 * as root, finish their privileged setup, then switch to their ID for good
 * (arb_privdrop.h). The arm binds only a connection whose effective user ID is
 * the guardian's, and the watchdog accepts each role only from that role's ID,
 * so being root, or being first, is no longer enough to pass as the guardian.
 *
 * The range 800-899 is clear of the image's own users: mkqnximage numbers its
 * users from 1000, and its security-policy mode reserves 900 and 901.
 */
#ifndef ARB_IDS_H
#define ARB_IDS_H

#define ARB_UID_OPERATOR   800   /* the local operator: the only user who may reset SAFE_STOP (FR-G5) */
#define ARB_UID_GUARDIAN   810
#define ARB_UID_ARM        811
#define ARB_UID_WATCHDOG   812
#define ARB_UID_LOGD       813   /* M2 P6 */
#define ARB_UID_GATEWAY    814   /* M2 P6 */
#define ARB_UID_UNTRUSTED  820   /* untrusted test tools, e.g. the F11 attacker arb-untrusted */

/* The guardian may query exactly the arm and the watchdog (one range, arb_privdrop.h). */
_Static_assert(ARB_UID_WATCHDOG == ARB_UID_ARM + 1, "the guardian's XPROCESS_QUERY range is arm..watchdog");

#endif /* ARB_IDS_H */
