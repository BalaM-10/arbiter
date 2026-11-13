# Architecture

Status: M1 in progress. This page describes what is built: the decision logic
in `qnx/libcontract/`, the guardian in `qnx/guardian/` and the arm controller in
`qnx/arm/`. The full M1 design (scheduling, motion leases, timeouts, fault
containment) is in `docs/DESIGN_M1_ADDENDUM.md`; the watchdog is added here once
implemented. Requirement IDs are defined in
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
| 3 | Sequence number newer than the last (32-bit wrap-safe) | `SEQUENCE` | no |
| 4 | Token bucket on the guardian's own clock (the sender's timestamp is never used) | `RATE` | no |
| 5 | Every target within its joint limits | `JOINT_LIMIT` | **yes** |
| — | The arm's reported position is a sane angle (first check that needs it) | `INTERNAL` | no |
| 6 | Velocity: if any joint would exceed `vmax`, the duration is stretched to the minimum that keeps every joint at or under `vmax` (verdict `CLAMP`) | — | no |
| 7 | Keep-out zones along the whole path (below) | `KEEPOUT` | **yes** |

Checks 1 to 5 need no arm position, so a flood of invalid commands never causes
a query to the arm; the guardian asks the arm for its position only for commands
that pass them, which the rate limit bounds. A command counts as accepted only
once the arm has accepted its setpoint (`arb_guard_commit_accept()`); if the arm
refuses it (`STALE_START`) or does not answer in time (`ARM_FAULT`, which also
enters `SAFE_STOP`), it is reported as a DENY and is not a violation.

Only joint-limit and keep-out denials count toward the violation threshold: N
of them within W ms enter `SAFE_STOP` (owner decision, Nov 13). A DENY never
carries a setpoint.

**States (FR-G5).** The guardian starts in `HOLD`. An accepted command (ALLOW or
CLAMP) moves `HOLD` to `NORMAL`. No accepted command for longer than the
silence timeout moves `NORMAL` to `HOLD`; denied commands do not count as
activity. When the last client of `/dev/arbiter` closes or dies, `NORMAL` moves to
`HOLD` at once (`arb_guard_client_lost()`). `SAFE_STOP` is entered on the violation threshold or an external fault
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

## The guardian process (`arb-guardian`)

A single-threaded resource manager at `/dev/arbiter`, at FIFO priority 64
(`qnx/common/arb_prio.h`). It loads the contract table once and refuses to start
on any problem (exit 2, FR-C4); it also refuses to start if the arm's internal
name is not there. Clients send each command as a `devctl(ARB_DCMD_SUBMIT)` and
get the verdict back in the same call; `ARB_DCMD_GET_STATUS` reports state and
counters.

For each devctl the handler checks, before deciding: the descriptor was opened
for writing; the message is not part of a combine message; the size actually
sent (`ctp->info.srcmsglen`, `ctp->size`, never the client's own `nbytes`) is
exactly one command; the client's reply buffer can take the whole verdict.
Anything else is refused with `EBADF`, `EBADMSG` or `ENOTTY` and counted. Then:

```
checks 1-5 (no arm needed) --deny--> reply DENY
   | pass
ask the arm for its position     (TimerTimeout-bounded; failure -> SAFE_STOP, DENY ARM_FAULT)
checks 6-7 from that position --deny--> reply DENY
   | ALLOW or CLAMP
send the setpoint to the arm     (TimerTimeout-bounded; timeout -> SAFE_STOP, DENY ARM_FAULT;
   |                              arm says start is stale -> DENY STALE_START)
   | arm accepted
commit: AI not silent, HOLD -> NORMAL; reply ALLOW/CLAMP
```

A 1 ms timer pulse runs the silence timeout. When the last client closes or
dies, the close handler moves `NORMAL` to `HOLD` at once.

**The planner must keep talking.** Silence is measured from the last accepted
command, not from the end of a move: a planner that sends one long move and then
goes quiet for longer than the contract's silence timeout (250 ms in
`contracts/default.yaml`) is put in `HOLD`, and the arm stops along its path. A
planner keeps a long motion going by re-sending its target (or the next waypoint)
faster than that. This is FR-G5 as written: silence is a fault, whatever the arm
is doing at the time.

Not yet in the guardian (M1 days 4 to 6): motion-lease renewals and heartbeats,
the HOLD setpoint on silence, system logging through `slog2`, and the operator
reset.

## The arm controller (`arb-arm`)

A single thread at FIFO priority 70, the highest of all Arbiter threads. It
registers the internal name `arbiter_arm` with `name_attach()` on its own
channel (no `_NTO_CHF_UNBLOCK`, so a guardian whose send times out is released
by the kernel), loads the contract table, and runs a 1 ms timer pulse.

- **Motion** (`arm_motion.c`, pure): one straight joint-space segment at a time,
  both joints arriving together. Position and velocity are a function of the
  arm's clock, so extra or spoofed tick pulses cannot make it move faster (TM-C1).
  Stopping decelerates along the current segment (10 rad/s² for the fastest
  joint), so a stop never leaves the path the guardian checked.
- **Its own checks** on every setpoint, independent of the guardian: the sender
  is the bound guardian connection; the target is within the joint limits; the
  start is within 5 control cycles at `vmax` of where the arm actually is
  (otherwise `STALE_START`); the duration is stretched so no joint exceeds `vmax`.
  The new segment starts from the actual position.
- **Motion lease:** each setpoint is valid for 10 ms unless the guardian renews
  it with a pulse; the guardian renews every tick unless it is in `SAFE_STOP`.
  An expired lease brakes and latches; only a new setpoint moves the arm again.
  If the guardian's connection disappears (closed or the process died), the
  kernel's disconnect pulse brakes the arm at once.
- **Hold and brake:** the guardian sends a hold pulse on entering `HOLD` (stop
  along the path, not latched) and a brake pulse on entering `SAFE_STOP`
  (latched).
- **Telemetry** (`arb_telemetry.h`): one sample per cycle into a shared-memory
  ring that only the arm writes; readers map it read-only and never block it.
- **Backend** (`arm_backend.h`): the loop calls only `init`, `apply` and `read`;
  M1's backend is the simulation.

## What is not covered by `libcontract`

- It decides commands; it does not move anything. Whether the arm follows the
  decided path is the arm controller's job (M1, `docs/DESIGN_M1_ADDENDUM.md`).
- Collisions of the links themselves with a zone (only the elbow and tip points
  are checked), self-collision, and obstacles not in the contract.
- Timing: `libcontract` has no clock. Deadlines are measured on the target, and
  timing is claimed only from Raspberry Pi 5 runs.
