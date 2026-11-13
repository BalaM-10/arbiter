# Architecture

Status: M1 in progress. This page describes what is built (the decision logic in
`qnx/libcontract/`). The guardian-to-arm design (scheduling, motion leases,
timeouts, fault containment) is in `docs/DESIGN_M1_ADDENDUM.md` and is added
here once implemented. Requirement IDs are defined in
`docs/REQUIREMENTS_SUMMARY.md`.

## Components and trust

| Component | Runs on | Trust | Job |
| --- | --- | --- | --- |
| Guardian (`arb-guardian`) | QNX | Trusted | Decides every command with `libcontract`; the only process allowed to send setpoints to the arm (TM-C2, `docs/THREAT_MODEL.md`) |
| Arm controller (`arb-arm`) | QNX | Trusted | 1 kHz loop driving a simulated 2-joint arm |
| Watchdog (`arb-watchdog`) | QNX | Trusted | Detects guardian or arm death and missed heartbeats; forces a stop |
| Evidence logger (`arb-logd`) | QNX | Trusted, low priority | Hash-chained decision log (M2) |
| Gateway (`arb-gateway`) | QNX | Untrusted | TCP from the planner to the guardian (M2) |
| Planner, contract compiler, bench | Host | Untrusted / build-time / test | Python tools |

Only the guardian, the arm controller, the watchdog and `libcontract` are <!-- not a claim -->
trusted code (the trust list is T1 to T6 in `docs/THREAT_MODEL.md`). They follow the rules in `rt-c-review`: no allocation after
startup, no recursion, every loop bounded, every input validated first.

## Units

Everything that crosses a trust boundary or is stored in the contract is an
integer: angles in microradians, lengths in micrometres, velocities in µrad/s,
durations in milliseconds. Untrusted input can therefore never be NaN or
infinite. `libcontract` uses `double` internally only for kinematics, computed
from integers that have already been range-checked.

## The contract table

The YAML contract is compiled on the host into a fixed 156-byte binary table
(`qnx/common/arb_contract_table.h`). The guardian never parses text (FR-C5).
`arb_contract_load()` refuses a table on any of: wrong length, magic, version or
size field, CRC-32 mismatch, or a value outside its allowed range (FR-C4). One
cross-field rule: `max_duration_ms` must be at least the time a full-range move
takes at the velocity limit, so a velocity CLAMP can never need a longer move
than the contract allows.

## How a command is decided

`arb_guard_check()` runs these checks in order; the first that fails decides
(FR-G4):

| # | Check | Rule ID on failure | Counts toward SAFE_STOP |
| --- | --- | --- | --- |
| 1 | Exactly 32 bytes; version 1; flags and reserved zero | `MALFORMED` | no |
| 1 | Requested duration ≥ 1 ms and ≤ the contract's `max_duration_ms` | `DURATION` | no |
| 2 | Not in `SAFE_STOP` | `STATE` | no |
| — | The arm's reported position is a sane angle | `INTERNAL` | no |
| 3 | Sequence number newer than the last (32-bit wrap-safe) | `SEQUENCE` | no |
| 4 | Token bucket on the guardian's own clock (the sender's timestamp is never used) | `RATE` | no |
| 5 | Every target within its joint limits | `JOINT_LIMIT` | **yes** |
| 6 | Velocity: if any joint would exceed `vmax`, the duration is stretched to the minimum that keeps every joint at or under `vmax` (verdict `CLAMP`) | — | no |
| 7 | Keep-out zones along the whole path (below) | `KEEPOUT` | **yes** |

Only joint-limit and keep-out denials count toward the violation threshold: N
of them within W ms enter `SAFE_STOP` (owner decision, Nov 13). A DENY never
carries a setpoint.

**States (FR-G5).** The guardian starts in `HOLD`. An accepted command (ALLOW or
CLAMP) moves `HOLD` to `NORMAL`. No accepted command for longer than the
silence timeout moves `NORMAL` to `HOLD`; denied commands do not count as
activity. `SAFE_STOP` is entered on the violation threshold or an external fault
(arm-send timeout, watchdog, emergency stop) and is left only by an explicit
operator reset, which returns to `HOLD`. Replay protection survives a reset.

## Path checking

The arm moves along a straight line in joint space from its current position to
the target, with both joints arriving together. The guardian checks exactly
that line, for the elbow and the tip, at 129 evenly spaced samples.

Between two samples the joints move by at most s₁ and s₂ radians, so the tip
moves at most D = (L₁ + L₂)·s₁ + L₂·s₂ and the elbow at most L₁·s₁ ≤ D. Any
point between two samples lies within D/2 of one of them. Each zone is therefore
inflated by D/2 + 1 µm before the samples are tested: if no sample is inside an
inflated zone, no point of the path is inside the real zone. The margin grows
with the length of the move; for a 0.5 rad move of joint 1 it is about 1.1 mm.

## Leaving a keep-out zone the arm starts in

The arm can start a move with the elbow or tip inside a zone (inflated by that
move's margin): for example if it was stopped there, or if a new contract adds
a zone around its current pose. Two mechanisms apply:

1. **An exit-only move is allowed.** A move that starts inside a zone is allowed
   only if, at every sample, each point already inside a zone gets no deeper in
   that zone than at the previous sample, no point enters a zone it was not in,
   and the final pose is outside every inflated zone. Such a verdict carries the
   flag `ARB_VFLAG_KEEPOUT_EXIT`, and the move does not count as a violation.
   Between two samples a point can be at most D/2 deeper than at the nearer
   sample; that bound is the same one the margin uses.
2. **Any other move from inside is denied** with rule `KEEPOUT` and counts as a
   violation. Repeated attempts reach the threshold and enter `SAFE_STOP`.

**Operator reset does not move the arm.** It only leaves `SAFE_STOP` for `HOLD`.
After a reset with the arm still inside a zone, the only way out is an exit-only
move. There is no override that lets a non-exit move through.

Tests: `fr_g4_keepout_exit_only_move_from_inside_allowed`,
`fr_g4_keepout_from_inside_going_deeper_denied`,
`fr_g4_keepout_from_inside_not_ending_outside_denied`,
`fr_g4_keepout_exit_through_another_zone_denied` in
`qnx/tests/test_libcontract.c`.

## What is not covered by `libcontract`

- It decides commands; it does not move anything. Whether the arm follows the
  decided path is the arm controller's job (M1, `docs/DESIGN_M1_ADDENDUM.md`).
- Collisions of the links themselves with a zone (only the elbow and tip points
  are checked), self-collision, and obstacles not in the contract.
- Timing: `libcontract` has no clock. Deadlines are measured on the target, and
  timing is claimed only from Raspberry Pi 5 runs.
