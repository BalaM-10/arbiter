# Architecture

Status: M1 in progress. This page describes what is built: the decision logic
in `qnx/libcontract/`, the guardian in `qnx/guardian/` and the arm controller in
`qnx/arm/` and the watchdog in `qnx/watchdog/`. The full M1 design (scheduling,
motion leases, timeouts, fault containment) is in `docs/DESIGN_M1_ADDENDUM.md`.

**Startup order: watchdog, arm, guardian.** Each later process refuses to start
if the one before it is not there. Requirement IDs are defined in
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
that line at 129 evenly spaced samples, and at each sample it tests both links
as line segments: the upper arm (base to elbow) and the forearm (elbow to tip).
Until 2026-11-16 only the elbow and tip points were tested, which let an allowed
move lay the forearm across a zone with both points outside (midpoint review,
B1); `fr_g4_keepout_forearm_across_zone_denied_though_elbow_and_tip_clear` is
that case.

**Depth of a link.** The depth of a point in a zone is its smallest distance to
the zone's four sides (> 0 inside). The depth of a link is the largest depth of
any point on it. Along the segment each of the four distances is linear, so the
largest "smallest of four" lies at an end or where two of them cross: at most 8
candidate points, computed exactly (`link_depth()` in `arb_contract.c`).

Between two samples the joints move by at most s₁ and s₂ radians, so no point of
either link moves more than the tip does, D = (L₁ + L₂)·s₁ + L₂·s₂. Every point a
link sweeps between two samples lies within D/2 of that link at one of them. Each
zone is therefore inflated by D/2 + 1 µm before the samples are tested: if no
sampled link meets an inflated zone, no point of the arm is inside the real zone. The margin grows
with the length of the move; for a 0.5 rad move of joint 1 it is about 1.1 mm.

## Leaving a keep-out zone the arm starts in

The arm can start a move with a link inside a zone (inflated by that move's
margin): for example if it was stopped there, or if a new contract adds a zone
around its current pose. Two mechanisms apply:

1. **An exit-only move is allowed.** A move that starts with a link inside a zone
   is allowed only if, at every sample, each link inside a zone is strictly
   shallower in that zone than at the previous sample, no link enters a zone it
   was not in, and the final pose has both links outside every inflated zone
   (owner decision 2026-11-16). Such a verdict carries the flag
   `ARB_VFLAG_KEEPOUT_EXIT`, and the move does not count as a violation. Between
   two samples a link can be at most D/2 deeper than at the nearer sample; that
   bound is the same one the margin uses. Limits: a link may still move sideways
   inside the zone while its deepest point gets shallower; and the difference
   between "strictly shallower" and "not deeper" shows only at exactly equal
   depths, which a continuous path almost never produces, so no test isolates it.
2. **Any other move from inside is denied** with rule `KEEPOUT` and counts as a
   violation. Repeated attempts reach the threshold and enter `SAFE_STOP`.

**Operator reset does not move the arm.** It only leaves `SAFE_STOP` for `HOLD`.
After a reset with the arm still inside a zone, the only way out is an exit-only
move. There is no override that lets a non-exit move through.

A zone that comes within 20 mm of the arm's base is refused by the contract
compiler (`fr_c2_zone_at_the_arm_base_rejected`): the upper arm always touches
the base, so the arm could never leave such a zone.

Tests: `fr_g4_keepout_exit_only_move_from_inside_allowed`,
`fr_g4_keepout_from_inside_going_deeper_denied`,
`fr_g4_keepout_from_inside_not_ending_outside_denied`,
`fr_g4_keepout_exit_through_another_zone_denied`,
`fr_g4_keepout_forearm_starting_across_zone_exits_allowed`,
`fr_g4_keepout_forearm_from_inside_going_deeper_denied`,
`fr_g4_keepout_forearm_turning_back_in_on_the_way_out_denied`,
`fr_g4_keepout_forearm_from_inside_not_ending_clear_denied`,
`fr_g4_keepout_link_exit_through_another_zone_denied` in
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

Added during M1: motion-lease renewals and watchdog heartbeats on every tick,
and the hold pulse to the arm on entering HOLD. Added in M2: system logging
through `slog2` (NFR-Q6, `qnx/common/arb_log.h`): startup, every state change with
its cause, and faults, written after the reply, never while deciding. Also in
M2: the watchdog's own failure (FR-W5, FR-W6, see the watchdog section), the
emergency stop and the operator reset.

**Emergency stop and operator reset (FR-W4, FR-G5, decision D2).** Two devctl
commands on `/dev/arbiter`, run by the operator console `arb-ctl`:

- `ARB_DCMD_ESTOP` is accepted from **any** client, even on a read-only
  descriptor: stopping is always allowed. The guardian enters `SAFE_STOP` with
  cause `ESTOP` and sends the arm its brake pulse before it replies.
- `ARB_DCMD_RESET` is accepted only from the **operator**: a descriptor opened for
  writing whose effective user ID, as the kernel reports it
  (`ConnectClientInfo()`), is the operator user ID given to `arb-guardian` at
  startup. Without that argument every reset is refused. Root is not the operator.
  The gateway (M2 P6) will run under its own user ID, so a reset sent through it
  is refused (to be tested in P6). Only in `SAFE_STOP`. The guardian first sends the arm a reset message
  (bounded, 10 ms; the arm accepts it only from its bound guardian): the arm
  registers again with the watchdog and only then clears its latch, and refuses
  when no watchdog answers. Then the guardian registers again and goes to `HOLD`.
  So after the watchdog was lost, motion resumes only once a watchdog runs again
  **and** the operator resets (decision 8). The reset is the only path that
  blocks the guardian longer than a command's two sends; it exists only in
  `SAFE_STOP`, when nothing moves.
- Until the per-process users of M2 P5, every Arbiter process runs as root; the
  operator check is by user ID, so it holds already (the tests use uid 800 for
  the operator and show root refused).

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

## The watchdog (`arb-watchdog`)

A single thread at FIFO priority 66: above the guardian, so a guardian spinning
in a loop cannot starve it (TM-C3), and below only the arm's control loop. It never
sends a message to a client, so no client can block it.

- **Registration.** The arm and the guardian each register a role with it, and
  hand it a pulse event aimed at their own channel, carrying a random token and
  registered with `MsgRegisterEvent()` so that only the watchdog may deliver it.
  A role held by a live process cannot be taken over (`EBUSY`; TM-C4).
- **Stuck.** Each client sends a heartbeat pulse every cycle; no heartbeat for
  10 ms from that role's own connection means stuck. Heartbeats sent on any
  other connection are ignored.
- **Died.** The process manager sends a death notice for every process
  (`procmgr_event_notify_add`). The notice is delivered through the watchdog's
  own connection, whose server connection ID it learns from its first timer
  pulse before its name exists; a pulse with the same code from any other
  connection is a forgery and is ignored. When a client exits, its disconnect
  pulse often arrives first and trips at once; the death notice then records the
  cause as `DIED`.
- **Trip.** A dead or stuck guardian: the watchdog delivers the arm's event and
  the arm brakes and latches (FR-W2). A dead or stuck arm: it delivers the
  guardian's event and the guardian enters `SAFE_STOP` with cause `WATCHDOG`.
  Every trip is recorded in a read-only status record: role, cause, pid, time.
- **Its own failure is fail-safe (FR-W5, FR-W6, M2).** Every 2 ms the watchdog
  delivers each client a second registered event, its beat. The arm and the
  guardian each stop on their own when the watchdog is lost: the arm brakes and
  latches, the guardian enters `SAFE_STOP` with cause `WATCHDOG`. Lost means either
  the kernel reports that the watchdog's channel is gone (`_PULSE_CODE_COIDDEATH`
  on the client's connection, confirmed with `ConnectServerInfo()`: it died), or
  10 of the client's own 1 ms ticks pass without a beat (it is stopped, stuck or
  dead). A watchdog that runs again restarts nothing. A watchdog spinning at
  priority 66 would starve the guardian (64); the arm (70) still brakes.
- The watchdog cannot physically stop anything; a hung arm controller with <!-- not a claim -->
  powered motors needs a hardware cut-off (`docs/THREAT_MODEL.md`, O3).

## Users, privileges and who serves a name (NFR-I5, FR-G6, F11)

Every Arbiter process runs as its own user (`qnx/common/arb_ids.h`; each user
ID is also the process's only group). The trusted processes start as root, do
the setup that needs root, then call `arb_privdrop()` (`qnx/common/arb_privdrop.c`)
once, on the main thread, before they talk to any other Arbiter process. It
keeps a short list of abilities for the non-root user, denies and locks every
other one with `procmgr_ability()`, switches real, effective and saved user and
group IDs, and then checks itself: `setuid(0)`, a priority one above the kept
range and `fork()` must each be refused with `EPERM`, and setting its own
priority must still work. If any check fails, the process exits (status 8)
rather than run half-dropped.

| Process | User | Done as root, before the drop | Kept after the drop | Why each kept ability |
| --- | --- | --- | --- | --- |
| `arb-watchdog` | 812 | FIFO 66, status memory, channel, death-notice registration, name `arbiter_watchdog` | PRIORITY 1-66 | Keeps its own 66, nothing above: it still delivers the arm's events at 70 (measured under the security policy, which allows it only 1-66: FR-W2 and FR-W5 pass). No CONNECTION: delivering events its clients registered needs none (measured: FR-W2 passes without it) |
| `arb-arm` | 811 | FIFO 70, telemetry memory, channel, name `arbiter_arm` | PRIORITY 1-70, CONNECTION, XPROCESS_QUERY for user 812 | Keeps its own 70; heartbeat pulses to the watchdog, another user (`MsgSendPulse()` needs CONNECTION); reading the watchdog's `/proc/<pid>/ctl` to check who serves its name |
| `arb-guardian` | 810 | FIFO 64, channel, `/dev/arbiter` (`resmgr_attach()` needs PATHSPACE) | PRIORITY 1-64, CONNECTION, XPROCESS_QUERY for users 811-812 | Keeps its own 64; lease, hold and brake pulses to the arm and heartbeats to the watchdog, other users; checking who serves the arm's and the watchdog's names |

Two facts were measured on QEMU before being relied on: `procmgr_ability()`
applies a range only when ALLOW and SUBRANGE are in one entry (an ALLOW entry
followed by a separate SUBRANGE entry left every priority allowed), and the range
is inclusive (`evidence/runs/2026-11-15-qemu/privdrop-subrange-probe.log`);
XPROCESS_QUERY limited to users 811-812 lets a process read a uid-811 process's
`/proc/<pid>/ctl` and still refuses a uid-820 one
(`evidence/runs/2026-11-15-qemu/procfs-query-probe.log`).

**Whose connection the arm accepts.** The arm binds the first connection whose
effective user ID is the guardian's (810), through the guardian's first state
query, and refuses every other connection. The guardian drops to 810 before it
sends that query, so neither root nor a process that connects first can bind
the arm. The watchdog accepts the guardian's role only from 810 and the arm's
only from 811.

**Who serves a name (F11 case 4).** Any process may register a name such as
`arbiter_arm` before the real one starts, and `name_open()` then reaches it.
So before a trusted client sends anything over a connection it opened by name
(`arm_link_open()`; `arb_wd_client_register()`, also used on operator reset),
`arb_server_check()` asks the kernel which process owns the channel
(`ConnectServerInfo()`), reads that process's user IDs from `/proc/<pid>/ctl`
(`DCMD_PROC_INFO`), and accepts the server only if its real and effective user
IDs are the expected one. A server of another user cannot even be queried (TM-C2)
(XPROCESS_QUERY covers exactly the users checked), so it fails the check, the
client logs who it found and refuses to start, and the impostor receives no
message. On the operator-reset path this runs only in SAFE_STOP, with the arm
braked. With the security policy loaded, the name itself can be attached only
by the process's own type (M2 P5, the security-policy image).

## What is not covered by `libcontract`

- It decides commands; it does not move anything. Whether the arm follows the
  decided path is the arm controller's job (M1, `docs/DESIGN_M1_ADDENDUM.md`).
- The links' width: they are checked as lines, so a contract must add their
  half-width to each zone. Also self-collision, and obstacles not in the
  contract.
- Timing: `libcontract` has no clock. Deadlines are measured on the target, and
  timing is claimed only from Raspberry Pi 5 runs.
