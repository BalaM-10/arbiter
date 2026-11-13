# Requirements summary

The requirement IDs cited in code, tests, commit messages and
`docs/QNX_EVIDENCE.md`, each with a one-line description. Every requirement
needs at least one automated test that names its ID. Requirements that say what
an untrusted party cannot do cite the claim in `docs/THREAT_MODEL.md` (TM-C1 to TM-C8).

## Hackathon constraints (HC)

| ID | Description |
| --- | --- |
| HC-1 | Submit before the deadline set by the official rules |
| HC-2 | The project runs on or targets QNX 8.0 |
| HC-3 | The repo is public and open source, with a license GitHub detects (Apache-2.0) |
| HC-4 | The repo shows the project genuinely builds for or runs on QNX 8.0 |
| HC-5 | Build and run instructions name the QNX version, the target and the toolchain |
| HC-6 | A demo video under 3 minutes, public on YouTube, shows the project on its target |
| HC-7 | The write-up and video stand on their own, since judges may score from them alone |
| HC-8 | The project is new, with real commit history inside the submission period |
| HC-9 | The work is original and solely owned; third-party code only under compatible licenses, listed in `NOTICE` |
| HC-10 | No third-party trademarks or copyrighted music in the video |
| HC-11 | Eligible for one overall prize, one bonus prize and Community Choice |

## Safety contract (FR-C)

| ID | Description |
| --- | --- |
| FR-C1 | One YAML contract file holds joint limits, velocity limit, keep-out zones, silence timeout, rate limit, maximum move duration, and violation threshold with its time window |
| FR-C2 | The contract compiler rejects missing, out-of-range or contradictory values with a clear message |
| FR-C3 | The compiler emits a fixed-layout binary table with a format version, a CRC and the source file's SHA-256 |
| FR-C4 | The guardian loads the table once at startup and refuses to start on a version or CRC mismatch |
| FR-C5 | The guardian never parses text; all parsing happens on the host |
| FR-C6 | The contract hash appears in every evidence packet |

## Guardian (FR-G)

| ID | Description |
| --- | --- |
| FR-G1 | The guardian is a QNX resource manager at `/dev/arbiter`, receiving each command as a `devctl()` over native message passing |
| FR-G2 | A command carries a sequence number, a sender timestamp, target angles for each joint, and a requested duration to reach them; zero or above the contract maximum is denied |
| FR-G3 | Each command gets one verdict, `ALLOW`, `CLAMP` (duration stretched so no joint exceeds the velocity limit) or `DENY`, with the ID of the deciding rule |
| FR-G4 | Checks run in order: well-formed, newer sequence, rate limit, joint limits, velocity limit, keep-out zones |
| FR-G5 | States `NORMAL`, `HOLD` (AI silent) and `SAFE_STOP` (needs an explicit operator reset); only joint-limit and keep-out denials count toward the violation threshold |
| FR-G6 | Only the guardian can send setpoints to the arm controller, enforced by sender identity and by the security policy (TM-C2) |
| FR-G7 | The guardian emits a heartbeat every control cycle |
| FR-G8 | Every decision and its measured latency go to a ring buffer without blocking |
| FR-G9 | After initialization: no memory allocation, no unbounded loop, no blocking call except the message receive and one `TimerTimeout()`-bounded setpoint send to the arm, whose timeout enters `SAFE_STOP` |

## Arm controller (FR-A)

| ID | Description |
| --- | --- |
| FR-A1 | A 1 kHz control loop driven by a QNX timer |
| FR-A2 | A simulated 2-joint planar arm with position and velocity per joint |
| FR-A3 | The arm moves toward the last approved setpoint, never faster than the contract velocity limit |
| FR-A4 | The arm brakes on its own if guardian heartbeats stop for a configured number of cycles |
| FR-A5 | The backend sits behind one interface, so a servo backend can replace the simulation |
| FR-A6 | Joint state is published every cycle for telemetry |

## Watchdog (FR-W)

| ID | Description |
| --- | --- |
| FR-W1 | The watchdog runs above the guardian and below only the arm's control loop, and monitors guardian and arm heartbeats |
| FR-W2 | On a missed guardian heartbeat it commands the arm to brake and records the cause |
| FR-W3 | It detects process death through process-manager death notices and stuck processes through missed heartbeats |
| FR-W4 | An emergency-stop input (dashboard or GPIO button) forces `SAFE_STOP` |
| FR-W5 | If the watchdog itself dies, the guardian enters `SAFE_STOP` and the arm brakes (fail-safe); the cause is recorded (decided Nov 13, M2) |
| FR-W6 | The watchdog sends its own heartbeat to the arm and the guardian; if it stops (a frozen watchdog), the arm brakes and the guardian enters `SAFE_STOP` (decided Nov 13, M2) |

## Gateway (FR-N)

| ID | Description |
| --- | --- |
| FR-N1 | The gateway accepts one planner connection over TCP and forwards well-formed commands to the guardian |
| FR-N2 | The wire format is a fixed-size binary frame, documented in `docs/PROTOCOL.md` |
| FR-N3 | A crash, hang or flood of the gateway cannot delay the guardian, the arm or the watchdog (TM-C3, TM-C4) |
| FR-N4 | The gateway streams joint state, verdicts and state changes to the host for the dashboard |

## Evidence (FR-E)

| ID | Description |
| --- | --- |
| FR-E1 | Each record holds sequence, monotonic time, command, verdict, rule ID, decision latency and guardian state |
| FR-E2 | Records are hash-chained with SHA-256 |
| FR-E3 | An evidence packet bundles the log, contract hash, build IDs, target description and fault bench results |
| FR-E4 | `arbiter evidence verify` recomputes the chain and fails loudly on any edit, gap or reorder |
| FR-E5 | Losing the logger never affects a decision; dropped records are counted and reported |

## Planner (FR-P)

| ID | Description |
| --- | --- |
| FR-P1 | A scripted planner replays a fixed pick-and-place path, with no API key or network |
| FR-P2 | An LLM planner turns a plain-language task into joint targets; provider and model are configuration |
| FR-P3 | The planner is never given the contract limits as hard constraints; it must be able to be wrong |
| FR-P4 | API keys come from the environment only and never enter the repo |

## Fault bench (FR-B)

| ID | Description |
| --- | --- |
| FR-B1 | `arbiter bench run` runs every fault scenario against a live target and writes JSON and HTML reports |
| FR-B2 | Each scenario states its pass condition up front and reports pass or fail with supporting records |
| FR-B3 | The bench runs unattended and exits non-zero on any failure |

## Dashboard (FR-D)

| ID | Description |
| --- | --- |
| FR-D1 | A live 2D view shows the arm, the keep-out zone, and commanded versus actual position |
| FR-D2 | A feed lists each verdict with its rule, colored by outcome |
| FR-D3 | A histogram shows guardian decision latency with the deadline marked |
| FR-D4 | Controls trigger each fault scenario and the emergency stop |
| FR-D5 | The current state is always visible |
| FR-D6 | The dashboard is read-only toward safety: it cannot change a limit or bypass the guardian (TM-C1, TM-C2) |

## Security policy (FR-S)

| ID | Description |
| --- | --- |
| FR-S1 | Each Arbiter process runs under its own QNX security type |
| FR-S2 | Only the guardian's type may connect to the arm controller; the gateway's type may connect only to the guardian (TM-C2) |
| FR-S3 | Only the guardian's type may attach `/dev/arbiter` (TM-C2) |
| FR-S4 | The compiled policy is in the boot image and pushed before any Arbiter process starts |
| FR-S5 | The policy source is in the repo, and the policy inspection tool's output is saved in `evidence/` |
| FR-S6 | All fault scenarios still pass with the policy off, so the code-level checks stand on their own |

## Timing (NFR-T)

Targets, not results. They are revised after the first Raspberry Pi 5
measurement, and timing is claimed only from Pi 5 runs saved in `evidence/`.

| ID | Description |
| --- | --- |
| NFR-T1 | Control loop period: target 1 ms |
| NFR-T2 | Worst-case guardian decision time over 1 million commands: target under 200 µs |
| NFR-T3 | Violating command to braking setpoint: target within 2 control cycles |
| NFR-T4 | AI silence timeout before `HOLD`: 250 ms, set in the contract |
| NFR-T5 | Guardian death to arm braking: target within 5 control cycles |
| NFR-T6 | Loop jitter with the gateway flooded and a low-priority CPU hog: target no worse than 2x idle |
| NFR-T7 | Every published number states hardware, sample count, min, median, 99.9th percentile and max |

## Isolation and robustness (NFR-I)

| ID | Description |
| --- | --- |
| NFR-I1 | Each component is its own process; no trusted process shares memory with the gateway |
| NFR-I2 | Trusted processes lock their memory and pre-fault their stacks at startup |
| NFR-I3 | Trusted processes use fixed-priority FIFO scheduling with documented priorities |
| NFR-I4 | Killing any untrusted process leaves the arm safe within the timeouts above |
| NFR-I5 | Trusted processes drop every privilege they do not need after startup |
| NFR-I6 | Malformed input of any length or content cannot crash a trusted process (TM-C8) |

## Evidence of QNX use (NFR-Q)

| ID | Description |
| --- | --- |
| NFR-Q1 | `docs/QNX_EVIDENCE.md` maps every QNX feature used to its file, function and reason |
| NFR-Q2 | Command and setpoint paths use synchronous message passing; heartbeats and timer ticks use pulses |
| NFR-Q3 | The guardian is a real resource manager, visible in the path namespace |
| NFR-Q4 | Decision latency is measured with the QNX high-resolution cycle counter |
| NFR-Q5 | A kernel trace of a fault scenario is captured as supporting evidence |
| NFR-Q6 | Logs go through the QNX system logger |

## Quality (NFR-K)

| ID | Description |
| --- | --- |
| NFR-K1 | C code is C11 and builds with the QNX compiler, warnings as errors, for x86_64 and aarch64 |
| NFR-K2 | The contract checks are a pure library with no OS calls, unit-tested on the host and on QNX |
| NFR-K3 | A clean clone builds and runs the QEMU demo by following the README alone |
| NFR-K4 | Python code has type hints, passes a linter, and has tests for the compiler and verifier |
| NFR-K5 | No secret, credential or QNX-licensed file is ever committed |

## Fault scenarios (F)

| ID | Description |
| --- | --- |
| F1 | Normal operation: every command `ALLOW`, the arm reaches each target, no deadline miss |
| F2 | AI hang: `HOLD` within the silence timeout, then back to `NORMAL` when commands resume |
| F3 | AI flood: a message flood and a pulse flood; excess commands `DENY`, unauthorized pulses ignored, no trusted priority changes, loop jitter within NFR-T6 |
| F4 | Joint limit violation: `DENY` by the joint-limit rule; the arm never exceeds the limit |
| F5 | Keep-out violation: `DENY` by the keep-out rule; elbow and end effector never enter the zone |
| F6 | Over-speed: `CLAMP`; measured joint velocity never exceeds the limit |
| F7 | Repeated violations: `SAFE_STOP`, brake, stay stopped until operator reset |
| F8 | Malformed input: truncated, oversized and random frames rejected; nothing trusted crashes or stalls |
| F9 | Replay: old sequence numbers `DENY` by the sequence rule |
| F10 | AI process killed: `HOLD`, then safe; trusted processes unaffected |
| F11 | Bypass attempt: a direct setpoint to the arm, also from a process started before the guardian, is blocked by the security policy, and rejected by the arm with the policy off, whatever the startup order (TM-C2) |
| F12 | CPU starvation attempt: latency and jitter stay within NFR-T2 and NFR-T6 |
| F13 | Guardian killed: the arm brakes within NFR-T5 and the watchdog records the cause |
| F14 | Emergency stop: `SAFE_STOP` within 2 control cycles |
| F15 | Evidence tampering: `arbiter evidence verify` fails and names the first bad record |
