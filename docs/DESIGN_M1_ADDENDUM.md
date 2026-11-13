# M1 design addendum: guardian, arm and watchdog working together

Status: **approved by the owner on 2026-11-13**, with the settlements recorded
in "Decisions" at the end (priority mapping, `name_attach()` for the arm, the
measured send timeout, pulse floods in F3). Implementation starts from here.

Ground rules for this document:

- Every QNX API assumption cites the QNX SDP 8.0 page it rests on. Where a page
  was not conclusive, the item is marked **to confirm on target**, with the test
  that settles it.
- Timing is reported as **measurements, never guarantees.** Numbers named here
  are *budgets to be measured against*. QEMU runs prove function only; timing
  claims come only from Raspberry Pi 5 runs saved in `evidence/`.
- Requirement IDs: `docs/REQUIREMENTS_SUMMARY.md`. Acceptance tests are named
  after the requirement or fault scenario they cover.
- The attacker model and the claims (TM-C1 to TM-C8) are in `docs/THREAT_MODEL.md`.

Page abbreviations used below (all under `https://www.qnx.com/developers/docs/8.0/`):

| Short name | Page |
| --- | --- |
| [TimerTimeout] | `com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html` |
| [ChannelCreate] | `com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html` |
| [ChannelCtl] | `com.qnx.doc.neutrino.lib_ref/topic/c/channelctl.html` |
| [ConnectClientInfo] | `com.qnx.doc.neutrino.lib_ref/topic/c/connectclientinfo.html` |
| [MsgSendPulse] | `com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html` |
| [_pulse] | `com.qnx.doc.neutrino.lib_ref/topic/p/_pulse.html` |
| [dispatch_create_channel] | `com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create_channel.html` |
| [PI-messages] | `com.qnx.doc.neutrino.sys_arch/topic/ipc_Priority_inheritance_messages.html` |
| [Sync-messaging] | `com.qnx.doc.neutrino.sys_arch/topic/ipc_Sync_messaging.html` |
| [Server-boost] | `com.qnx.doc.neutrino.sys_arch/topic/ipc_Server_boost.html` |
| [Client-termination] | `com.qnx.doc.neutrino.prog/topic/process_Client_termination.html` |
| [timer_create] | `com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html` |
| [timer_settime] | `com.qnx.doc.neutrino.lib_ref/topic/t/timer_settime.html` |
| [sigevent] | `com.qnx.doc.neutrino.lib_ref/topic/s/sigevent.html` |
| [Timing] | `com.qnx.doc.neutrino.prog/topic/timing.html` |
| [procnto] | `com.qnx.doc.neutrino.utilities/topic/p/procnto.html` |
| [on] | `com.qnx.doc.neutrino.utilities/topic/o/on.html` |
| [Abilities] | `com.qnx.doc.security.system/topic/manual/abilities_reference.html` |
| [mlockall] | `com.qnx.doc.neutrino.lib_ref/topic/m/mlockall.html` |
| [ClockCycles] | `com.qnx.doc.neutrino.lib_ref/topic/c/clockcycles.html` |
| [procmgr_event_notify_add] | `com.qnx.doc.neutrino.lib_ref/topic/p/procmgr_event_notify_add.html` |
| [MsgRegisterEvent] | `com.qnx.doc.neutrino.lib_ref/topic/m/msgregisterevent.html` |
| [slay] | `com.qnx.doc.neutrino.utilities/topic/s/slay.html` |
| [name_attach] | `com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html` |
| [name_open] | `com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html` |
| [ClockPeriod] | `com.qnx.doc.neutrino.lib_ref/topic/c/clockperiod.html` |
| [SDP8-RN] | `https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.release_notes/topic/sdp8_rn.html` |

---

## 1. SAFE_STOP semantics: requested, achieved, confirmed

"Safe stop" is used at three levels, and every log line, report and narration
must say which one it means.

| Level | Meaning | Who establishes it | In scope |
| --- | --- | --- | --- |
| **L1 Stop requested** | A stop was decided: the guardian entered `SAFE_STOP`, or the watchdog tripped, or the arm's motion lease expired. Recorded with its cause and a `ClockCycles()` timestamp | Guardian / watchdog / arm | Yes |
| **L2 Simulated stop achieved** | The arm controller's model reports zero velocity on both joints and a held position | Arm controller, from its own model; checked independently by the bench from telemetry | Yes |
| **L3 Physical stop confirmed** | Real motors de-energized or braked, confirmed by an independent sensor | — | **No.** Needs an independent hardware cut-off (for example a safe-torque-off relay) and stop sensing, which this prototype does not have. With the optional servo stretch, nothing beyond L2 is claimed |

**Brake model (simulated).** On any stop, the arm decelerates along its current
straight joint-space segment at a fixed deceleration from the arm model, and
never leaves that segment. Because the guardian already checked the whole
segment, braking cannot enter a keep-out zone (TM-C1). Stopping distance is therefore
inside the checked path by construction.

**What is reported.** For every stop: cause, L1 time, L2 time, and the measured
number of control cycles from L1 to L2. No document says "the arm stopped"
without the level.

**API assumptions.** `ClockCycles()` is a free-running counter and its rate is
`SYSPAGE_ENTRY(qtime)->cycles_per_sec`; the hardware behind it is synchronized
across CPUs [ClockCycles]. On x86_64 the startup-determined rate "is not exact
and may vary from boot to boot" [ClockCycles], so QEMU cycle counts convert to
time only approximately; another reason QEMU numbers are not timing evidence.

**Acceptance tests.**
- `fr_g5_safe_stop_records_request_and_achieved_times` (on target): force a fault
  mid-move; the evidence shows L1 and L2 times and the cycle count between them.
- `fr_a3_brake_stays_on_segment` (unit, arm model): braking from any point of a
  segment at any speed ends on that segment with zero velocity.
- Bench check in F7, F13, F14: the bench recomputes L2 from telemetry itself and
  fails if the arm's own L2 claim disagrees.

## 2. Local motion expiry (the motion lease)

**Rule.** Every motion authorization **expires inside the arm** unless renewed.
A dead or hung guardian therefore never leaves a move valid, and the design
never relies only on delivering a STOP to a process that may not be listening.

- Each setpoint the arm accepts starts a lease: `lease_until = now + LEASE`.
  Proposed `LEASE` = 10 control cycles (10 ms at 1 ms), a contract value to
  measure against (this is FR-A4's "configured number of cycles").
- The guardian renews the lease every tick with a pulse to the arm. The arm
  accepts a renewal only from the connection it authorized: a pulse carries the
  sender's server connection ID (`scoid`) [_pulse], which the arm compares with
  the guardian's.
- The arm checks the lease in its own control loop, driven by its own timer. On
  expiry it brakes (L1 → L2) and **latches**: later renewals do not revive the
  old move; only a new setpoint does.
- Independent, redundant paths to a stop, none of which needs the guardian to be
  responsive:
  1. Lease expiry in the arm (local, no message needed).
  2. Guardian disconnect: the arm's channel uses `_NTO_CHF_DISCONNECT`, so the
     kernel sends the arm a pulse when all of the guardian's connections are
     detached, including when the guardian dies ("If a process dies without
     detaching all its connections, the kernel detaches them for it")
     [ChannelCreate]. The arm brakes at once.
  3. Watchdog: death notice or missed guardian heartbeat → brake pulse to the arm
     (FR-W2, section 6).

**What is NOT covered.**
- **The arm controller itself hung or dead.** In the simulation a dead arm
  process means a frozen model (no motion), which is trivially "stopped"; on
  real hardware a hung controller with powered motors needs a hardware cut-off,
  which is out of scope (L3 above). The watchdog detects and records the arm's
  death or missed heartbeat but cannot physically stop anything. <!-- not a claim -->
- Failure of the arm's own timer or of the kernel/clock.
- A compromised trusted process (the guardian sending valid but harmful setpoints
  is exactly what `libcontract` prevents; a guardian whose code is replaced is not).

**API assumptions.** Periodic timer delivering a pulse: `timer_create()` with a
`SIGEV_PULSE` event; "the event doesn't need to be registered" [timer_create].
Stopping a process for the hung test: `slay -h` sets SIGSTOP and `slay -u` sets
SIGCONT [slay].

**Acceptance tests (on target).**
- `fr_a4_lease_expires_when_guardian_stopped`: `slay -h arb-guardian` mid-move;
  the arm brakes; report measured cycles from the last renewal to L1 and to L2.
- `fr_a4_lease_expires_when_guardian_killed`: kill the guardian mid-move; the
  disconnect pulse or the lease, whichever comes first, brakes the arm; report
  which and the measured cycles.
- `fr_a4_renewal_from_other_connection_ignored`: a test client connected to the
  arm sends renewal pulses; the lease still expires.
- `fr_a4_renewal_after_expiry_does_not_resume`: after expiry, renewals alone do
  not move the arm.

## 3. Bounding the guardian's setpoint send (FR-G9)

**Rule.** The guardian's one blocking call besides its receive, the setpoint
`MsgSend()` to the arm, is bounded with `TimerTimeout()`, armed **immediately
before** the call, covering **both** the SEND-blocked and the REPLY-blocked
states. A timeout is a fault: `arb_guard_fault(ARB_STOP_ARM_TIMEOUT)` →
`SAFE_STOP`.

```
TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns, NULL);
rc = MsgSend(arm_coid, &setpoint, sizeof setpoint, &reply, sizeof reply);   /* nothing in between */
if (rc == -1 && errno == ETIMEDOUT) -> fault ARM_TIMEOUT
```

**API facts this relies on** [TimerTimeout]:
- MsgSend's blocking states are "STATE_SEND or STATE_REPLY", selected with
  `_NTO_TIMEOUT_SEND` and `_NTO_TIMEOUT_REPLY`.
- The timeout applies to "the next kernel call made by the same thread" and is
  disarmed "when it exits from the kernel". Hence: no other kernel call (no log
  write, no clock read through a kernel call) between `TimerTimeout()` and
  `MsgSend()`. A code check enforces it (below).
- `CLOCK_MONOTONIC` is allowed; a NULL event means `SIGEV_UNBLOCK`; on expiry the
  call fails with `ETIMEDOUT`. Without `TIMER_ABSTIME` the time is relative.
- **Design consequence:** if the server's channel has `_NTO_CHF_UNBLOCK` and has
  already received the message, the client is *not* released at the timeout;
  "the kernel sends an unblock pulse to the server, and it's up to the server to
  do a MsgReply*() or MsgError()". **The arm's channel is therefore created
  without `_NTO_CHF_UNBLOCK`**, so a REPLY-blocked guardian is released by the
  kernel even if the arm never replies.
- Granularity: timers are aligned to the tick, 1 ms by default [Timing], so the
  timeout fires at the first tick after it expires. Sub-tick timeouts would need
  `PROCMGR_AID_HIGH_RESOLUTION_TIMER` [TimerTimeout]; M1 does not use them.

**Measured on QEMU (function only, not timing evidence)** with
`qnx/tests/test_send_timeout.c`, 2 ms requested, n = 1000 per case. The tick read
with `ClockPeriod()` and `clock_getres()` is 1 ms:

| Case | min | median | 99.9th pct | max | Result |
| --- | --- | --- | --- | --- | --- |
| SEND: server never receives | 2.357 ms | 2.996 ms | 3.718 ms | 3.718 ms | every send `ETIMEDOUT` |
| REPLY: received, never replied, channel without `_NTO_CHF_UNBLOCK` | 2.393 ms | 2.996 ms | 3.608 ms | 3.608 ms | every send `ETIMEDOUT` |
| Same with `_NTO_CHF_UNBLOCK`, server replies 20 ms later (n = 20) | 23.498 ms | 24.007 ms | 24.703 ms | 24.703 ms | client held until the server answered |

So the 2 ms timeout is supported: it never fired early and fired by the second
tick after expiry. The **effective bound is 2 to 4 ms**, and that is the value
used when budgeting the guardian's worst case. The third row confirms on target
why the arm's channel must not set `_NTO_CHF_UNBLOCK`. Raw output:
`evidence/runs/` run log of `test_send_timeout`. The Pi 5 value is measured in M3.

The same bound applies to the guardian's state query to the arm (section 5).

**Acceptance tests (on target, with a stub arm, not test hooks in the real arm).**
- `fr_g9_arm_never_replies_times_out_and_safe_stops`: stub arm receives the
  setpoint and never replies (REPLY-blocked case). Guardian's send fails with
  `ETIMEDOUT`; guardian enters `SAFE_STOP` with cause `ARM_TIMEOUT`; measured
  time from send to fault is reported.
- `fr_g9_arm_not_receiving_times_out_and_safe_stops`: stub arm stopped with
  `slay -h` (SEND-blocked case). Same expectations.
- `fr_g9_timer_timeout_immediately_precedes_send`: a host-side source check that
  every `MsgSend()` in `qnx/guardian/` is directly preceded by `TimerTimeout()`.

## 4. Scheduling

**Final order (owner decision, 2026-11-13), highest first:**

**arm controller loop (70) > watchdog (66) > guardian (64) > evidence logger (30) > gateway and every untrusted process (≤ 20)**

- **The arm's control loop is highest**, so the 1 kHz loop, the lease check and
  the brake never wait for any other Arbiter process.
- **The watchdog is above the guardian**, so a guardian spinning in a loop
  cannot starve the watchdog that is meant to catch it (TM-C3, TM-C4).
- **The three trusted processes sit at 64 and above, the privileged range.**
  "The maximum unprivileged priority is usually 63, but is governed by the -P
  option to procnto" [Abilities]; "priorities of 64 and greater are privileged,
  so only processes with the PROCMGR_AID_PRIORITY ability enabled ... can use
  them" [on]. An untrusted process therefore cannot (TM-C3) run, or send a message or
  pulse, at a priority equal to or above any trusted thread. This matters for
  pulses: a server's priority is "boosted ... when a higher-priority pulse is
  sent" [PI-messages], and the gateway holds a connection to the guardian
  (it opens `/dev/arbiter`), so it can send pulses there. Kept below 64, it
  cannot boost the guardian above the watchdog or the arm (TM-C3). The three trusted
  processes keep `PROCMGR_AID_PRIORITY` when they drop their other privileges
  (NFR-I5, M2).
- Priorities 254 and 255 belong to the clock and IPI interrupt service threads
  [procnto], above everything here by design. The kernel is a strict-priority
  scheduler with FIFO, round-robin and sporadic policies [SDP8-RN]; every Arbiter
  thread uses FIFO.
- **Consequence of the arm being highest:** if the arm's loop itself spun
  forever, it would starve the watchdog, which could then not record it. The
  arm's per-cycle work is bounded by construction (`rt-c-review`), and a faulty
  trusted process is out of scope (O4 in `docs/THREAT_MODEL.md`). <!-- not a claim -->

| Thread | Priority (FIFO) | Activation | Period | Work budget per activation (to measure) | Channel flags |
| --- | --- | --- | --- | --- | --- |
| Arm controller loop | 70 | Own timer pulse; guardian messages and pulses; watchdog brake pulse | 1 ms | ≤ 50 µs per tick; ≤ 30 µs per setpoint | Own channel created with `_NTO_CHF_FIXED_PRIORITY \| _NTO_CHF_DISCONNECT \| _NTO_CHF_COID_DISCONNECT`, **not** `_NTO_CHF_UNBLOCK`; registered with `name_attach()` |
| Watchdog | 66 | Own timer pulse; heartbeat pulses; death-notice pulses | 1 ms | ≤ 20 µs | Heartbeat channel: `_NTO_CHF_FIXED_PRIORITY`. Death-notice channel: `_NTO_CHF_PRIVATE` |
| Guardian | 64 | devctl from clients; own 1 ms tick pulse | 1 ms tick; commands as they come (rate-limited by the contract) | Decision ≤ 200 µs (NFR-T2 target); tick ≤ 20 µs; arm send bounded at 2 to 4 ms (section 3) | `/dev/arbiter` resource manager channel: `_NTO_CHF_FIXED_PRIORITY \| _NTO_CHF_UNBLOCK \| _NTO_CHF_DISCONNECT` |
| Evidence logger (M2) | 30 | Own timer | 10 ms | Bounded batch per drain | — |
| Gateway (M2) and test clients | 20 | Network / test input | — | Untrusted; no budget | — |
| Shells, sshd, other system processes | as configured by the image (to record with `pidin` on target) | | | | |

**To confirm on target (test `nfr_i3_untrusted_cannot_send_above_its_priority`):**
that a process without `PROCMGR_AID_PRIORITY` cannot send a pulse (TM-C3) with a
priority above 63 (the [MsgSendPulse] page states only the general range).

**Why `_NTO_CHF_FIXED_PRIORITY` on each channel.** By default "when the server
receives a message, its effective priority changes to that of the
highest-priority sender", it "does not revert ... when the thread replies", and
it is lowered "when a message is received from a lower-priority thread or a
lower-priority pulse is received" [PI-messages]. The flag suppresses this:
"Receiving threads won't change their priorities to those of the sending
threads" [ChannelCreate].
- Guardian: its clients run at 20. Without the flag the guardian would drop to
  20 while handling them, and a CPU hog at 20 (F12) would compete with it.
- Arm: its main client is the guardian at 64. Without the flag the arm's loop
  thread would drop from 70 to 64 after each setpoint, changing loop timing with
  the guardian's activity.
- Watchdog: heartbeats come from 64 and 70; the watchdog must stay at 66.

**To confirm on target.** The flag is documented for messages; the pages do not
say whether it also stops *pulse*-driven priority changes [ChannelCreate],
[PI-messages]. Mitigation either way: every pulse is sent at the receiving
thread's own priority (timer events via `SIGEV_PULSE_INIT` [sigevent],
heartbeats and renewals via `MsgSendPulse(coid, receiver_priority, ...)`
[MsgSendPulse]). Test: `nfr_i3_receiver_priority_unchanged_by_pulses_and_messages`
reads each trusted thread's priority after a burst of messages and pulses from
lower-priority senders. If pulses do lower a receiver despite the flag, the
fallback is that each trusted thread restores its own priority after any pulse
from a lower-priority sender (a bounded call), tested by the same test.

**Timer period.** The default tick is 1 ms and "the expiry of system timers is
aligned to the tick" [Timing]; at most 50 timer events are generated per tick
[timer_create]. A 1 ms periodic timer on a 1 ms tick is expected to work but its
period distribution must be measured, not assumed. Test: `fr_a1_loop_period_measured`
records every period for 60 s and reports min, median, 99.9th percentile and max
(NFR-T7). If 1 ms is not met on the Pi 5, stop and report (rule 10).

**Memory.** "In this QNX OS release, all memory is locked for the entire duration
of the process, so this function does nothing" [mlockall]. Trusted processes
still call `mlockall()` (it documents intent and costs nothing) and pre-fault
their stacks, but NFR-I2 is satisfied by the OS, and the docs say so.

**Tested under flood and CPU load:** see section 6.

## 5. Motion model: a new command arrives mid-move

The arm executes at most **one active segment**: a straight line in joint space
from a start position to a target over a duration, with both joints arriving
together (FR-A3).

1. The guardian asks the arm for its current position (a short message, bounded
   like the setpoint send, section 3).
2. `libcontract` decides the new command from that position.
3. On ALLOW/CLAMP the guardian sends `SETPOINT{start = the position it checked
   from, target, duration, guardian sequence}`.
4. The arm compares `start` with its actual position. If any joint differs by
   more than `START_TOL` (proposed: the distance at `vmax` in 5 control cycles,
   5 mrad at 1 rad/s) it rejects the setpoint as stale; the guardian reports a
   DENY with a new rule `STALE_START`, not counted as a violation.
5. Otherwise the new segment **replaces** the active one immediately, starting
   from the arm's *actual* position. Latest wins: there is no queue.
6. The arm recomputes the duration from its own start and its own copy of the
   contract's `vmax`, and uses the longer of that and the guardian's duration.
   The arm enforces the velocity limit independently of the guardian (FR-A3).

**Consequence for keep-out.** The executed segment starts up to `START_TOL` away
(in joint space) from the checked one and converges to the same target, so every
executed point lies within `START_TOL` per joint of the checked path. In the
workspace that is at most (L₁ + 2·L₂)·`START_TOL` (4 mm for 5 mrad). This extra
margin is added to the keep-out margin in `libcontract` (a change to
`arb_guard_check`, with tests).

**Other cases.**
- A **DENY** mid-move does not touch the active segment: the arm continues the
  last approved move, under its lease.
- **HOLD** (AI silent): the guardian sends a hold setpoint (target = current
  position, decelerate along the segment) and keeps renewing the lease, since
  holding is authorized.
- **SAFE_STOP**: the guardian sends a brake and stops renewing; the arm latches
  braked until a new setpoint after an operator reset.
- M1 has no acceleration or jerk limits apart from the brake deceleration; a new
  segment can change direction instantly in the simulation. Stated as a
  limitation of the simulated arm.

**Acceptance tests.**
- `fr_a3_new_setpoint_mid_move_is_straight_from_actual_position` (on target):
  telemetry after a mid-move setpoint is a straight joint-space line from the
  actual position, with every joint at or under `vmax`.
- `fr_a3_stale_start_rejected` (on target, stub guardian): a setpoint whose start
  is more than `START_TOL` from the actual position is rejected and motion is
  unchanged.
- `fr_a3_deny_mid_move_keeps_active_segment` (on target).
- `fr_g4_keepout_margin_includes_start_tolerance` (unit, `libcontract`).

## 6. Fault containment: invalid-command flood while the arm moves

**Claim to test (TM-C3):** a flood of invalid commands while the arm is moving cannot
starve the arm's control loop, the watchdog, or any stop path.

**Mechanisms.**
1. **Strict priorities** (section 4): the arm loop (70) and the watchdog (66)
   preempt the guardian (64) and every client (≤ 20) whenever they are ready
   [SDP8-RN]. Flood work happens at 64 or below.
2. **The guardian does not inherit its clients' priority**
   (`_NTO_CHF_FIXED_PRIORITY`), so it does not drop to 20; and since no
   untrusted sender can use a priority of 64 or more (section 4), none can push
   it above 64.
3. **Synchronous sends throttle the sender**: each command blocks its client
   until the guardian replies [Sync-messaging]; a single-threaded client has one
   command outstanding at a time.
4. **Bounded work per command**: `libcontract` rejects malformed input in O(1)
   and every check is bounded (`docs/ARCHITECTURE.md`); measured as part of NFR-T2.
5. **Renewals keep flowing**: the guardian's tick pulse is at priority 64, and "a
   server process receives messages and pulses in priority order"
   [PI-messages], so a tick is received ahead of queued client messages at 20.
   **To confirm on target:** the order among equal priorities is not stated.
6. **Stop paths need neither the guardian nor the flood source**: lease expiry is
   local to the arm (section 2); the watchdog's brake pulse to the arm is sent at
   the arm's priority.
7. **Pulse floods from untrusted processes.** Pulses queue from kernel memory;
   "If no memory is available to queue a pulse, the kernel fails the pulse
   request with an EAGAIN error" [MsgSendPulse]. A channel can have a bounded
   private pulse pool (`_NTO_CHF_PULSE_POOL` and `ChannelCtl()` with
   `_NTO_CCTL_PULSE_POOL`) [ChannelCreate], [ChannelCtl]. **To confirm in the docs
   before use:** the exhaustion behaviour of a pool (the page text I could verify
   does not state it). In M1 an untrusted process can still connect to the arm
   and queue pulses that the arm then ignores (its scoid is not authorized); the
   M2 security policy removes the connection itself (FR-S2, F11). The arm's name
   is visible to any process under `/dev/name/local/`, so in M1 an untrusted
   process can `name_open()` it; the pulse flood test therefore targets the arm
   as well as the guardian.

**Not covered in M1:** pulse floods before the M2 policy exists (mitigated only
by the pulse pool, if confirmed); kernel memory exhaustion by other means; a
flood from a *trusted* process; anything needing an acceleration-limited physical
arm.

**Acceptance tests (on target; QEMU for function, Pi 5 for numbers).**
- `f3_flood_while_moving_does_not_starve_loop_or_stop_path`: a test client at
  priority 20 sends malformed and replayed commands as fast as it can while the
  arm executes a 5 s move. Pass: no lease expiry (renewals keep up); every
  control-loop period and watchdog cycle recorded; a fault injected mid-flood
  reaches L2 in a measured number of cycles; the jitter comparison of NFR-T6
  is reported.
- `f3_pulse_flood_while_moving_does_not_starve_loop_or_stop_path` (owner
  decision: F3 covers pulses as well as messages): an untrusted test process
  floods pulses, at the highest priority it is allowed and with unknown and
  valid-looking codes, on every connection it can open (the guardian through
  `/dev/arbiter`, the arm through `name_open()`), while the arm moves. Pass: the
  same as the message flood, plus every trusted thread's priority unchanged,
  pulses from unauthorized connections ignored and counted, and any `EAGAIN`
  the flooder sees recorded.
- `f12_cpu_hog_at_client_priority_does_not_delay_trusted_threads`: the same
  with a busy-loop process at priority 20.
- `nfr_q5_trace_of_flood`: a `tracelogger` capture of the flood, showing the
  trusted threads running while the flooder is ready (`qnx-trace-capture`).

---

## Decisions (approved by the owner, 2026-11-13)

1. The three stop levels; L3 is out of scope (section 1).
2. Lease = 10 control cycles, renewed by guardian pulses; expiry latches (section 2).
3. Arm channel without `_NTO_CHF_UNBLOCK`; 2 ms send timeout, **measured on QEMU
   as an effective 2 to 4 ms** (section 3); Pi 5 value in M3.
4. **Priority order: arm 70 > watchdog 66 > guardian 64 > logger 30 > untrusted
   ≤ 20**; watchdog above the guardian; arm loop highest; trusted processes in the
   privileged range (section 4). Budgets are measurement targets.
5. Latest-wins single segment, `START_TOL` = 5 cycles at `vmax`, new rule
   `STALE_START`, and the extra keep-out margin (section 5).
6. The flood test design and the "not covered" list; **F3 includes a pulse flood
   as well as a message flood** (section 6).
7. **Channel discovery:** the arm registers an internal name with `name_attach()`
   for the guardian only; the guardian stays a resource manager at
   `/dev/arbiter`. With a NULL dispatch handle `name_attach()` would create its
   channel with `_NTO_CHF_UNBLOCK` ("The channel will be created with the
   _NTO_CHF_COID_DISCONNECT, _NTO_CHF_UNBLOCK, and _NTO_CHF_DISCONNECT flags set"
   [name_attach]), which section 3 rules out, so the arm creates its own channel
   with `ChannelCreate()`, wraps it with `dispatch_create_channel()`
   [dispatch_create_channel] and passes that handle to `name_attach()`.
   **To confirm on target:** that the returned `chid` is the arm's own channel
   (the page allows the handle but does not say the flags are kept), and how the
   arm must answer the connect message `name_open()` sends (the page does not
   say). Both are checked by `fr_g6_arm_name_attach_keeps_channel_flags`.
   `name_open()` gives "no guarantee that the client will end up talking to the
   server it thinks it's connecting to" [name_open]: an impostor that registered
   the name could receive the guardian's setpoints but could not move the real
   arm, whose lease then expires (TM-C4); in M2 the policy controls who may attach
   the name.
