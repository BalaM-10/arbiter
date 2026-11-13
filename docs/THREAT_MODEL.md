# Threat model

Status: draft, 2026-11-13 (milestone M1). Every claim in the documentation that
an untrusted party *cannot* do something refers to a claim ID below (TM-C1 to
TM-C8). `scripts/check-claims.py` fails if such a sentence has no reference.

## Scenario and what is protected

A tabletop arm moves parts on a shared workbench while a person works next to
it; an AI planner decides the moves. Arbiter protects one thing: **the arm stays
inside its safety contract** (joint limits, speed limit, the operator's hand area
as a keep-out zone, timing rules), and **comes to a stop when the system that
commands it fails**. It does not protect the task: a wrong or hostile planner
can make the arm do nothing useful, and can always force a hold or a safe stop.
That outcome is accepted, because it is safe.

## Trust boundary

```
 untrusted                         |  trusted (QNX target)
                                   |
 AI planner (host) --TCP--> gateway --devctl--> guardian --setpoint--> arm controller --> simulated arm
                                   |               |  heartbeats            |
                                   |               +-------> watchdog <-----+
```

The boundary is the guardian's resource manager, `/dev/arbiter`. Everything to
its left is assumed hostile.

## What the attacker controls (in scope)

| ID | Capability |
| --- | --- |
| A1 | **Planner output.** Any targets, durations, sequence numbers and timestamps; any rate, from silence to a flood; plausible-but-wrong and deliberately adversarial plans (for example from a manipulated prompt) |
| A2 | **Gateway input.** Anything that can reach the gateway's TCP port can send what A1 can (M2) |
| A3 | **A compromised gateway or any other untrusted QNX process**, running arbitrary code with that process's user ID and abilities: any `devctl()` content, length and rate on `/dev/arbiter`; attempts to connect to any channel or path; pulses on any connection it obtains; CPU use at its own priority; crashing, hanging or exiting at any moment |
| A4 | **Killing or stopping any untrusted process** at any moment (the fault bench does this on purpose) |

## What is trusted

| ID | Trusted element | Why it is trusted |
| --- | --- | --- |
| T1 | Guardian, arm controller, watchdog, evidence logger, `libcontract` | Small, reviewed with `rt-c-review`, tested on QNX; they are the defence |
| T2 | The QNX kernel and process manager: scheduling, message passing, timers, death notices, security policy enforcement | The platform the design rests on; behaviour cited from the QNX 8.0 documentation and checked on target |
| T3 | The boot image, its startup script and the security policy it pushes (M2) | Built by the operator from this repository |
| T4 | The contract YAML and the compiler run on the operator's host | Whoever writes the contract defines safety; the CRC detects corruption, not malice |
| T5 | The operator, including the operator reset of `SAFE_STOP` | A human decision, by design |
| T6 | The QNX SDP toolchain and the host that builds the binaries | Outside the scope of a runtime safety monitor |

## Out of scope

| ID | Not defended against |
| --- | --- |
| O1 | **Root on the target**, or any process holding the abilities to change priorities, the security policy, or other processes' memory. The test images allow root SSH login with the developer's key for deployment; whoever holds that key is root |
| O2 | **Physical access**: serial console, swapping the SD card, touching the hardware |
| O3 | **Hardware faults**: CPU, memory, clock, timer, motor, sensor or power failure. A physical stop needs an independent hardware cut-off, which this prototype does not have (`docs/DESIGN_M1_ADDENDUM.md` section 1) |
| O4 | Malicious or faulty trusted code (T1 to T6), including the supply chain |
| O5 | Loss of availability: an attacker can always keep the arm in `HOLD` or force `SAFE_STOP` |
| O6 | Side channels, and any attack on the host machine other than through the planner's output |
| O7 | Whether the planner accomplishes its task |

## Claims

| ID | Claim | Against | Mechanism | Shown by | Status (2026-11-13) |
| --- | --- | --- | --- | --- | --- |
| TM-C1 | No command from the untrusted side moves the arm outside the contract: joint limits, speed limit, keep-out zones along the whole path, move duration | A1, A2, A3 | The guardian decides every command with `libcontract`; the arm independently enforces the speed limit, the start tolerance and the motion lease | F1, F4, F5, F6, F9; `qnx/tests/test_libcontract.c` | Decision logic tested on host and QEMU; end to end from M1 day 7 |
| TM-C2 | No untrusted process can get the arm to act on a setpoint | A3 | The arm accepts setpoints only from the connection whose user ID is the guardian's (M1); the kernel security policy refuses the connection itself (M2) | F11 with the policy on and off | Designed |
| TM-C3 | Untrusted message content, pulse floods, rate or CPU use cannot starve the arm's control loop, the watchdog or any stop path; their delays are measured, not assumed | A1, A3 | Strict fixed priorities with every trusted thread in the privileged range (64 and above) that no untrusted process can use; `_NTO_CHF_FIXED_PRIORITY` on trusted channels; synchronous sends that block the sender; bounded work per command (`docs/DESIGN_M1_ADDENDUM.md` sections 4 and 6) | F3 (message and pulse floods), F12 | Designed |
| TM-C4 | Losing the untrusted side leaves the arm safe: planner or gateway silence, death or hang leads to `HOLD`; guardian death or hang leads to a brake | A1, A3, A4 | Silence timeout; the resource manager's close handler; the arm's motion lease, disconnect pulse and the watchdog (addendum section 2) | F2, F10, F13 | Silence logic tested; the rest designed |
| TM-C5 | Replayed, reordered or out-of-date commands are refused; the sender's clock is never trusted | A1, A3 | Sequence numbers with wrap-safe comparison; rate limit on the guardian's own clock | F9; `fr_g4_sequence_*`, `fr_g4_rate_limit_uses_guardian_clock_not_sender_clock` | Tested on host and QEMU (decision logic) |
| TM-C6 | Editing, removing or reordering records in a saved evidence log is detected | Someone with write access to a saved log, but not to the anchor hash recorded elsewhere | SHA-256 hash chain, verified from a published anchor (M2) | F15 | Designed. A party who can rewrite a whole log **and** its anchor can forge it (O1) |
| TM-C7 | A corrupted or truncated contract table is never used | Accidental corruption, not an attacker (T4) | Version, size and CRC-32 checks at load; the guardian refuses to start | `fr_c4_*`, `test_contract_file` on QNX | Tested on host and QEMU |
| TM-C8 | Malformed input of any length or content cannot crash or stall a trusted process | A1, A3 | Length checked before any field is read; every field range-checked; integer-only inputs; bounded loops; no allocation (`rt-c-review`) | F8; `nfr_i6_*` in `qnx/tests/test_libcontract.c` | Decision logic tested on host (ASan, UBSan, 20 000 random inputs) and QEMU; the devctl handler from M1 day 3-4 |

## Known gaps today (M1)

- **Every process currently runs as root** in the test setup. Until privileges
  are dropped and the security policy is loaded (M2, NFR-I5, FR-S1 to FR-S6), a
  compromised untrusted process is effectively root, which is out of scope (O1).
  TM-C2 and TM-C3 are therefore not yet demonstrated against A3.
- Pulse floods from an untrusted process that has a connection to the arm are
  bounded only once a channel pulse pool is confirmed, or once the M2 policy
  removes the connection (addendum section 6).
- The gateway does not exist yet; A2 applies from M2.
- `name_open()` does not authenticate the server. A process that registered the
  arm's internal name first could receive the guardian's setpoints. It still
  could not move the real arm, whose motion lease would expire (TM-C4); from M2
  the security policy limits who may attach the name.
