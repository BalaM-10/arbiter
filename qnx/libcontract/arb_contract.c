/*
 * libcontract implementation. Pure: see arb_contract.h.
 * Every loop has a compile-time bound, stated beside it.
 */
#include <math.h>
#include <string.h>

#include "arb_arm_model.h"
#include "arb_contract.h"

#define NS_PER_MS          1000000ull
#define MILLI_PER_TOKEN    1000ull
#define REFILL_CAP_NS      (10ull * 1000000000ull) /* credit at most 10 s at once: bounds the multiply */
#define URAD_TO_RAD        1e-6

/* ------------------------------------------------------------------ CRC-32 */

uint32_t arb_crc32(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;

    /* Bound: len bytes x 8 bits; len is the caller's buffer size. */
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8u; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* ------------------------------------------------------------ table loading */

static int in_range_i32(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }
static int in_range_u32(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }

static int contract_values_ok(const arb_contract_table_t *t)
{
    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        if (!in_range_i32(t->joint_min_urad[j], -ARB_ANGLE_LIMIT_URAD, ARB_ANGLE_LIMIT_URAD) ||
            !in_range_i32(t->joint_max_urad[j], -ARB_ANGLE_LIMIT_URAD, ARB_ANGLE_LIMIT_URAD) ||
            t->joint_min_urad[j] >= t->joint_max_urad[j]) {
            return 0;
        }
    }
    if (!in_range_u32(t->vmax_urad_s, ARB_VMAX_MIN_URAD_S, ARB_VMAX_MAX_URAD_S) ||
        !in_range_u32(t->silence_timeout_ms, ARB_SILENCE_MIN_MS, ARB_SILENCE_MAX_MS) ||
        !in_range_u32(t->rate_per_s, ARB_RATE_MIN_PER_S, ARB_RATE_MAX_PER_S) ||
        !in_range_u32(t->rate_burst, ARB_BURST_MIN, ARB_BURST_MAX) ||
        !in_range_u32(t->violation_threshold, 1u, ARB_MAX_VIOLATION_THRESHOLD) ||
        !in_range_u32(t->violation_window_ms, ARB_WINDOW_MIN_MS, ARB_WINDOW_MAX_MS) ||
        !in_range_u32(t->max_duration_ms, ARB_DURATION_MIN_MS, ARB_DURATION_CEIL_MS) ||
        t->zone_count > (uint32_t)ARB_MAX_ZONES) {
        return 0;
    }
    /* max_duration_ms must allow a full-range move at vmax, so that a CLAMP of
     * any move between joint limits stays within it. Widened: span <= 2*pi*1e6. */
    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        const uint64_t span = (uint64_t)((int64_t)t->joint_max_urad[j] - (int64_t)t->joint_min_urad[j]);
        const uint64_t full_ms = (span * 1000u + (uint64_t)t->vmax_urad_s - 1u) / (uint64_t)t->vmax_urad_s;
        if (full_ms > (uint64_t)t->max_duration_ms) {
            return 0;
        }
    }
    for (uint32_t z = 0; z < (uint32_t)ARB_MAX_ZONES; z++) { /* bound: ARB_MAX_ZONES */
        const arb_zone_t *r = &t->zones[z];
        if (z >= t->zone_count) {
            /* Unused slots must be zero, so the table has one canonical form. */
            if (r->x_min_um != 0 || r->y_min_um != 0 || r->x_max_um != 0 || r->y_max_um != 0) {
                return 0;
            }
            continue;
        }
        if (!in_range_i32(r->x_min_um, -ARB_WORKSPACE_LIMIT_UM, ARB_WORKSPACE_LIMIT_UM) ||
            !in_range_i32(r->x_max_um, -ARB_WORKSPACE_LIMIT_UM, ARB_WORKSPACE_LIMIT_UM) ||
            !in_range_i32(r->y_min_um, -ARB_WORKSPACE_LIMIT_UM, ARB_WORKSPACE_LIMIT_UM) ||
            !in_range_i32(r->y_max_um, -ARB_WORKSPACE_LIMIT_UM, ARB_WORKSPACE_LIMIT_UM) ||
            r->x_min_um >= r->x_max_um || r->y_min_um >= r->y_max_um) {
            return 0;
        }
    }
    return 1;
}

arb_load_result_t arb_contract_load(const void *buf, size_t len, arb_contract_table_t *out)
{
    arb_contract_table_t t;

    if (buf == NULL || out == NULL) {
        return ARB_LOAD_E_ARG;
    }
    if (len != sizeof t) {                 /* length first, before reading any field */
        return ARB_LOAD_E_SIZE;
    }
    memcpy(&t, buf, sizeof t);             /* copy: no alignment assumption on buf */

    if (t.magic != ARB_CONTRACT_MAGIC) {
        return ARB_LOAD_E_MAGIC;
    }
    if (t.version != ARB_CONTRACT_VERSION) {
        return ARB_LOAD_E_VERSION;
    }
    if (t.table_size != (uint16_t)sizeof t) {
        return ARB_LOAD_E_TABLESIZE;
    }
    if (arb_crc32(&t, offsetof(arb_contract_table_t, crc32)) != t.crc32) {
        return ARB_LOAD_E_CRC;
    }
    if (!contract_values_ok(&t)) {
        return ARB_LOAD_E_RANGE;
    }
    *out = t;
    return ARB_LOAD_OK;
}

/* --------------------------------------------------------------- kinematics */

void arb_fk(const double q_rad[ARB_JOINTS], double elbow_um[2], double tip_um[2])
{
    const double a  = q_rad[0];
    const double ab = q_rad[0] + q_rad[1];

    elbow_um[0] = (double)ARB_LINK1_UM * cos(a);
    elbow_um[1] = (double)ARB_LINK1_UM * sin(a);
    tip_um[0]   = elbow_um[0] + (double)ARB_LINK2_UM * cos(ab);
    tip_um[1]   = elbow_um[1] + (double)ARB_LINK2_UM * sin(ab);
}

/* Depth of p inside zone z inflated by margin: > 0 inside, <= 0 outside. */
static double depth_in_zone(const double p[2], const arb_zone_t *z, double margin)
{
    double d = p[0] - ((double)z->x_min_um - margin);
    double e = ((double)z->x_max_um + margin) - p[0];

    if (e < d) { d = e; }
    e = p[1] - ((double)z->y_min_um - margin);
    if (e < d) { d = e; }
    e = ((double)z->y_max_um + margin) - p[1];
    if (e < d) { d = e; }
    return d;
}

typedef enum {
    KEEPOUT_CLEAR     = 0,  /* no point of the path is in any zone */
    KEEPOUT_EXIT_ONLY = 1,  /* starts inside a zone, never goes deeper, ends outside every zone */
    KEEPOUT_HIT       = 2   /* deny */
} keepout_result_t;

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

/*
 * Keep-out check of the straight joint-space path from `from` to `to` (the arm
 * moves along exactly this path) for the elbow and the tip.
 *
 * The path is sampled at ARB_PATH_SAMPLES + 1 points. Between two samples the
 * joints move by at most s1, s2 radians, so the tip moves at most
 * D = (L1 + L2) * s1 + L2 * s2 (and the elbow at most L1 * s1 <= D). Every point
 * between two samples lies within D / 2 of one of them, so inflating each zone
 * by D / 2 (plus 1 um for rounding) makes the sampled test conservative: if no
 * sample is inside an inflated zone, no point of the path is inside the zone.
 * The start-tolerance term below covers the arm starting from its actual position.
 *
 * Leaving a zone (docs/ARCHITECTURE.md): if the start pose already has a point
 * inside an inflated zone, the move is allowed only if, sample by sample, that
 * point's depth in that zone never increases, no other point enters any zone,
 * and the final pose is outside every inflated zone. Between two samples a
 * point can be at most D / 2 deeper than at the nearer sample.
 */
static keepout_result_t path_keepout(const arb_contract_table_t *c,
                                     const int32_t from[ARB_JOINTS], const int32_t to[ARB_JOINTS])
{
    const double d0 = (double)((int64_t)to[0] - (int64_t)from[0]) * URAD_TO_RAD;
    const double d1 = (double)((int64_t)to[1] - (int64_t)from[1]) * URAD_TO_RAD;
    const double s0 = fabs(d0) / (double)ARB_PATH_SAMPLES;
    const double s1 = fabs(d1) / (double)ARB_PATH_SAMPLES;
    const double step_um = ((double)ARB_LINK1_UM + (double)ARB_LINK2_UM) * s0 + (double)ARB_LINK2_UM * s1;
    /* The arm starts the executed segment from its actual position, which may be
     * up to tol (per joint) away from the checked start and converges to the same
     * target; every executed point is then within (L1 + 2 L2) * tol of the checked
     * path (docs/DESIGN_M1_ADDENDUM.md section 5). */
    const double tol_rad = (double)c->vmax_urad_s * (double)ARB_START_TOL_MS / 1000.0 * URAD_TO_RAD;
    const double start_tol_um = ((double)ARB_LINK1_UM + 2.0 * (double)ARB_LINK2_UM) * tol_rad;
    const double margin = step_um / 2.0 + start_tol_um + 1.0;
    double prev[ARB_MAX_ZONES][2];   /* depth at the previous sample: [zone][elbow, tip] */
    int started_inside = 0;

    if (c->zone_count == 0u) {
        return KEEPOUT_CLEAR;
    }
    for (int i = 0; i <= ARB_PATH_SAMPLES; i++) {          /* bound: ARB_PATH_SAMPLES + 1 */
        const double f = (double)i / (double)ARB_PATH_SAMPLES;
        double q[ARB_JOINTS];
        double pts[2][2];                                  /* elbow, tip */

        q[0] = (double)from[0] * URAD_TO_RAD + d0 * f;
        q[1] = (double)from[1] * URAD_TO_RAD + d1 * f;
        arb_fk(q, pts[0], pts[1]);
        for (uint32_t z = 0; z < c->zone_count && z < (uint32_t)ARB_MAX_ZONES; z++) { /* bound: ARB_MAX_ZONES */
            for (int k = 0; k < 2; k++) {                  /* bound: 2 points */
                const double d = depth_in_zone(pts[k], &c->zones[z], margin);
                if (i == 0) {
                    prev[z][k] = d;
                    if (d > 0.0) {
                        started_inside = 1;
                    }
                    continue;
                }
                if (prev[z][k] <= 0.0) {
                    if (d > 0.0) {
                        return KEEPOUT_HIT;                /* enters a zone it was not in */
                    }
                } else if (d > prev[z][k]) {
                    return KEEPOUT_HIT;                    /* goes deeper into a zone it starts in */
                }
                if (i == ARB_PATH_SAMPLES && d > 0.0) {
                    return KEEPOUT_HIT;                    /* does not end outside */
                }
                prev[z][k] = d;
            }
        }
    }
    return started_inside ? KEEPOUT_EXIT_ONLY : KEEPOUT_CLEAR;
}

/* ------------------------------------------------------------------- guard */

void arb_guard_init(arb_guard_t *g, const arb_contract_table_t *contract, uint64_t now_ns)
{
    memset(g, 0, sizeof *g);
    g->contract       = contract;
    g->state          = ARB_STATE_HOLD;   /* no AI command yet: hold */
    g->stop_cause     = ARB_STOP_NONE;
    g->tokens_milli   = (uint64_t)contract->rate_burst * MILLI_PER_TOKEN;
    g->refill_ns      = now_ns;
    g->last_accept_ns = now_ns;
}

static void enter_safe_stop(arb_guard_t *g, arb_stop_cause_t cause)
{
    if (g->state != ARB_STATE_SAFE_STOP) {
        g->state = ARB_STATE_SAFE_STOP;
        g->stop_cause = (uint8_t)cause;
    }
}

void arb_guard_fault(arb_guard_t *g, arb_stop_cause_t cause)
{
    enter_safe_stop(g, cause);
}

int arb_guard_reset(arb_guard_t *g, uint64_t now_ns)
{
    if (g->state != ARB_STATE_SAFE_STOP) {
        return -1;
    }
    g->state          = ARB_STATE_HOLD;
    g->stop_cause     = ARB_STOP_NONE;
    g->violation_next = 0;
    g->violation_fill = 0;
    g->last_accept_ns = now_ns;
    /* last_seq is kept: replay protection survives a reset. */
    return 0;
}

/* The divisions below need vmax and rate to be nonzero. arb_contract_load()
 * guarantees it; this re-check makes a guard on an unchecked table fail closed. */
static int contract_usable(const arb_contract_table_t *c)
{
    return c != NULL && c->vmax_urad_s >= ARB_VMAX_MIN_URAD_S && c->rate_per_s >= ARB_RATE_MIN_PER_S &&
           c->violation_threshold >= 1u && c->zone_count <= (uint32_t)ARB_MAX_ZONES;
}

int arb_guard_tick(arb_guard_t *g, uint64_t now_ns)
{
    uint64_t timeout_ns;

    if (!contract_usable(g->contract)) {
        return 0;
    }
    timeout_ns = (uint64_t)g->contract->silence_timeout_ms * NS_PER_MS;
    if (g->state == ARB_STATE_NORMAL && now_ns > g->last_accept_ns &&
        now_ns - g->last_accept_ns > timeout_ns) {
        g->state = ARB_STATE_HOLD;
        return 1;
    }
    return 0;
}

/* Serial-number arithmetic: is a newer than b, allowing for 32-bit wrap? */
static int seq_newer(uint32_t a, uint32_t b)
{
    const uint32_t d = a - b;              /* unsigned: wraps by definition */
    return d != 0u && d < 0x80000000u;
}

static void refill(arb_guard_t *g, uint64_t now_ns)
{
    const arb_contract_table_t *c = g->contract;
    const uint64_t cap = (uint64_t)c->rate_burst * MILLI_PER_TOKEN;
    uint64_t elapsed;
    uint64_t add;

    if (now_ns <= g->refill_ns) {
        return;
    }
    elapsed = now_ns - g->refill_ns;
    if (elapsed > REFILL_CAP_NS) {
        elapsed = REFILL_CAP_NS;           /* <= 1e10 * 1e4: no overflow below */
        g->refill_ns = now_ns - elapsed;
    }
    add = elapsed * (uint64_t)c->rate_per_s / 1000000ull;  /* milli-tokens */
    if (add == 0u) {
        return;                            /* keep refill_ns: the fraction is not lost */
    }
    /* Advance only by the time actually credited, so remainders carry over. */
    g->refill_ns += add * 1000000ull / (uint64_t)c->rate_per_s;
    g->tokens_milli += add;
    if (g->tokens_milli >= cap) {
        g->tokens_milli = cap;
        g->refill_ns = now_ns;
    }
}

/* Record a counted denial; return 1 if N of them now fall within the window. */
static int count_violation(arb_guard_t *g, uint64_t now_ns)
{
    const arb_contract_table_t *c = g->contract;
    const uint64_t window_ns = (uint64_t)c->violation_window_ms * NS_PER_MS;
    uint32_t in_window = 0;

    g->violation_ns[g->violation_next] = now_ns;
    g->violation_next = (g->violation_next + 1u) % ARB_MAX_VIOLATION_THRESHOLD;
    if (g->violation_fill < ARB_MAX_VIOLATION_THRESHOLD) {
        g->violation_fill++;
    }
    for (uint32_t i = 0; i < g->violation_fill; i++) {     /* bound: ARB_MAX_VIOLATION_THRESHOLD */
        const uint64_t t = g->violation_ns[i];
        if (t <= now_ns && now_ns - t < window_ns) {
            in_window++;
        }
    }
    return in_window >= c->violation_threshold;
}

static void deny(arb_verdict_t *out, arb_rule_t rule)
{
    out->verdict = ARB_VERDICT_DENY;
    out->rule = (uint8_t)rule;
    out->target_urad[0] = 0;
    out->target_urad[1] = 0;
    out->duration_ms = 0;
}

/* Steps 1-5: no arm position needed. Returns 1 if the command may proceed. */
int arb_guard_check_pre(arb_guard_t *g, const void *msg, size_t len, uint64_t now_ns,
                        arb_cmd_t *cmd, arb_verdict_t *out)
{
    const arb_contract_table_t *c = g->contract;

    memset(out, 0, sizeof *out);
    memset(cmd, 0, sizeof *cmd);
    out->state_before = g->state;
    out->state_after = g->state;

    if (!contract_usable(c)) {
        deny(out, ARB_RULE_INTERNAL);
        return 0;
    }

    /* 1. Well-formed. Length first, then every field, before any use. */
    if (msg == NULL || len != sizeof *cmd) {
        deny(out, ARB_RULE_MALFORMED);
        return 0;
    }
    memcpy(cmd, msg, sizeof *cmd);
    if (cmd->version != ARB_PROTO_VERSION || cmd->flags != 0u || cmd->reserved != 0u) {
        deny(out, ARB_RULE_MALFORMED);
        return 0;
    }
    out->seq = cmd->seq;
    out->requested_ms = cmd->duration_ms;
    /* Duration bounds: never zero, never above the contract's maximum. */
    if (cmd->duration_ms < ARB_DURATION_MIN_MS || cmd->duration_ms > c->max_duration_ms) {
        deny(out, ARB_RULE_DURATION);
        return 0;
    }

    /* 2. State gate: nothing moves in SAFE_STOP until an operator reset. */
    if (g->state == ARB_STATE_SAFE_STOP) {
        deny(out, ARB_RULE_STATE);
        return 0;
    }

    /* 3. Sequence: strictly newer than the last well-formed command (replay, F9). */
    if (g->have_seq && !seq_newer(cmd->seq, g->last_seq)) {
        deny(out, ARB_RULE_SEQUENCE);
        return 0;
    }
    g->have_seq = 1;
    g->last_seq = cmd->seq;

    /* 4. Rate limit: token bucket on the guardian's clock, not the sender's. */
    refill(g, now_ns);
    if (g->tokens_milli < MILLI_PER_TOKEN) {
        deny(out, ARB_RULE_RATE);
        return 0;
    }
    g->tokens_milli -= MILLI_PER_TOKEN;

    /* 5. Joint limits (counted toward the violation threshold). */
    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        if (cmd->target_urad[j] < c->joint_min_urad[j] || cmd->target_urad[j] > c->joint_max_urad[j]) {
            deny(out, ARB_RULE_JOINT_LIMIT);
            if (count_violation(g, now_ns)) {
                enter_safe_stop(g, ARB_STOP_VIOLATIONS);
            }
            out->state_after = g->state;
            return 0;
        }
    }
    return 1;
}

/* Steps 6-7: from the arm's position. Does not count the command as accepted. */
void arb_guard_check_motion(arb_guard_t *g, const arb_cmd_t *cmd,
                            const int32_t current_urad[ARB_JOINTS], uint64_t now_ns,
                            arb_verdict_t *out)
{
    const arb_contract_table_t *c = g->contract;
    uint64_t max_delta = 0;
    uint64_t min_ms;

    out->state_after = g->state;
    /* The arm's reported position must itself be sane; otherwise fail closed. */
    if (current_urad == NULL ||
        !in_range_i32(current_urad[0], -ARB_ANGLE_LIMIT_URAD, ARB_ANGLE_LIMIT_URAD) ||
        !in_range_i32(current_urad[1], -ARB_ANGLE_LIMIT_URAD, ARB_ANGLE_LIMIT_URAD)) {
        deny(out, ARB_RULE_INTERNAL);
        return;
    }

    /* 6. Velocity: the shortest duration that keeps every joint at or under vmax.
     *    Widened to 64 bits: |delta| <= 2 * ARB_ANGLE_LIMIT_URAD. */
    for (int j = 0; j < ARB_JOINTS; j++) {                 /* bound: ARB_JOINTS */
        const uint64_t d = (uint64_t)abs64((int64_t)cmd->target_urad[j] - (int64_t)current_urad[j]);
        if (d > max_delta) {
            max_delta = d;
        }
    }
    /* ceil(max_delta [urad] * 1000 / vmax [urad/s]) ms; vmax >= ARB_VMAX_MIN_URAD_S > 0 */
    min_ms = (max_delta * 1000u + (uint64_t)c->vmax_urad_s - 1u) / (uint64_t)c->vmax_urad_s;
    if (min_ms < ARB_DURATION_MIN_MS) {
        min_ms = ARB_DURATION_MIN_MS;
    }
    out->target_urad[0] = cmd->target_urad[0];
    out->target_urad[1] = cmd->target_urad[1];
    if ((uint64_t)cmd->duration_ms >= min_ms) {
        out->verdict = ARB_VERDICT_ALLOW;
        out->rule = ARB_RULE_NONE;
        out->duration_ms = cmd->duration_ms;
    } else if (min_ms <= (uint64_t)c->max_duration_ms) {
        out->verdict = ARB_VERDICT_CLAMP;
        out->rule = ARB_RULE_VELOCITY;
        out->duration_ms = (uint32_t)min_ms; /* <= max_duration_ms: fits */
    } else {
        /* Only possible if the arm is outside its joint limits; fail closed. */
        deny(out, ARB_RULE_DURATION);
        return;
    }

    /* 7. Keep-out zones along the whole path (counted toward the threshold).
     *    A keep-out failure overrides an ALLOW or CLAMP. */
    switch (path_keepout(c, current_urad, cmd->target_urad)) {
    case KEEPOUT_CLEAR:
        break;
    case KEEPOUT_EXIT_ONLY:
        out->flags |= ARB_VFLAG_KEEPOUT_EXIT;
        break;
    case KEEPOUT_HIT:
    default:
        deny(out, ARB_RULE_KEEPOUT);
        if (count_violation(g, now_ns)) {
            enter_safe_stop(g, ARB_STOP_VIOLATIONS);
        }
        out->state_after = g->state;
        return;
    }
}

void arb_guard_commit_accept(arb_guard_t *g, uint64_t now_ns, arb_verdict_t *out)
{
    /* Only an ALLOW or CLAMP that the arm accepted counts as activity. */
    if (out->verdict == ARB_VERDICT_ALLOW || out->verdict == ARB_VERDICT_CLAMP) {
        g->last_accept_ns = now_ns;
        if (g->state == ARB_STATE_HOLD) {
            g->state = ARB_STATE_NORMAL;
        }
    }
    out->state_after = g->state;
}

void arb_guard_reject_after_approval(arb_guard_t *g, arb_rule_t rule, arb_verdict_t *out)
{
    deny(out, rule);
    out->flags = 0;
    out->state_after = g->state;
}

int arb_guard_client_lost(arb_guard_t *g)
{
    if (g->state == ARB_STATE_NORMAL) {
        g->state = ARB_STATE_HOLD;
        return 1;
    }
    return 0;
}

void arb_guard_check(arb_guard_t *g, const void *msg, size_t len,
                     const int32_t current_urad[ARB_JOINTS], uint64_t now_ns,
                     arb_verdict_t *out)
{
    arb_cmd_t cmd;

    if (!arb_guard_check_pre(g, msg, len, now_ns, &cmd, out)) {
        return;
    }
    arb_guard_check_motion(g, &cmd, current_urad, now_ns, out);
    if (out->verdict != ARB_VERDICT_DENY) {
        arb_guard_commit_accept(g, now_ns, out);
    }
}
