# Arbiter

**The AI proposes. QNX decides.**

Arbiter is a QNX-based runtime safety prototype that validates AI-generated
motion commands against an explicit contract and demonstrates fault containment
for a defined set of failures, with reproducible test evidence.

It runs on **QNX OS 8.0**, between an untrusted AI planner and a (simulated)
robot arm. It is a safety-oriented design built for the QNX Global Hackathon; it
is not certified and not intended for production use.

**The scenario.** A tabletop arm moves parts on a shared workbench while a
person works next to it. An AI planner decides where each part goes. The
contract ([`contracts/default.yaml`](contracts/default.yaml)) marks the area
where the person's hands work as a keep-out zone, together with joint limits,
a speed limit and timing rules. Whatever the planner proposes, and whether it is
right, wrong, silent, flooding or dead, Arbiter decides on QNX what the arm may
do. What Arbiter defends against, and what it does not, is in
[`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md).

## Status: milestone M1 (safety core)

| Works today, on QNX 8.0 in QEMU | Not built yet |
| --- | --- |
| QNX SDP 8.0.4 toolchain, builds for x86_64 and aarch64le | Gateway and planner (M2) |
| QEMU x86_64 target built with `mkqnximage`, reached over SSH | Security policy, evidence logger (M2) |
| Scripted build, deploy and on-target test run, with every log saved | Fault scenarios F2, F3, F7, F8, F10 to F15 (M2) |
| `libcontract`: the guardian's decision logic (contract table check, command checks in order, keep-out along the whole path, states) as a pure library, 64 tests on the host and on QNX | Raspberry Pi 5 target, timing measurements, dashboard (M3) |
| Contract compiler (`arbiter contract`): strict YAML validation, binary table, checked against the C loader on the host and on QNX | |
| Guardian (`arb-guardian`): resource manager at `/dev/arbiter`, devctl verdicts, message checks, silence and client-loss HOLD, `TimerTimeout`-bounded arm link | |
| Arm controller (`arb-arm`): 1 kHz loop on its own timer, simulated 2-joint arm, its own speed and limit checks, guardian-only sender binding, 10 ms motion lease, brake on guardian loss, read-only telemetry; end to end with the guardian | |
| Watchdog (`arb-watchdog`): kernel death notices and heartbeats for guardian and arm; brakes the arm or stops the guardian through registered events; records which failure occurred | |
| **Fault bench** (`arbiter bench run`): F1 normal pick-and-place, F4 joint limit, F5 keep-out, F6 over-speed, F9 replay, run against the live target and judged from the arm's own telemetry with independent Python kinematics, not from the guardian's verdicts. [Latest report: 5/5](evidence/bench/2026-11-15-qemu/201321/report.html) | |
| Watchdog failure is fail-safe (M2): if the watchdog dies or freezes, the arm brakes and latches and the guardian enters SAFE_STOP, each on its own; a watchdog that comes back restarts nothing | |
| Emergency stop and operator reset (M2): `arb-ctl estop` from any client stops everything; `arb-ctl reset` works only for the operator's user ID, and only once a watchdog is running | |
| Isolation without the security policy (M2): every process runs as its own user; the trusted ones drop root and keep only the abilities they need, and check at startup that the rest is refused; the arm accepts only the guardian's user, and the guardian and the arm check who serves a name before trusting it. F11 (bypass, TM-C2) passes all four cases with the policy off; the policy image comes next | |
| System log (M2): guardian, arm and watchdog record startup, state changes, brakes, refused senders and watchdog trips through the QNX system logger, read on the target with `slog2info -b arb_guardian` (or `arb_arm`, `arb_watchdog`) | |
| Message-passing sample from M0: send/receive/reply, malformed messages, pulses, sender identity (7 checks) | |

On QEMU every result proves function only, never timing; timing claims will
come only from the Raspberry Pi 5 (M3).

## Quick start (QEMU)

Tested with QNX SDP 8.0.4 on Ubuntu 22.04 (WSL2 on Windows 11), QEMU 6.2,
toolchain `qcc` (GCC 12.2.0), target QEMU x86_64. Do the
[first-time setup](docs/TARGETS.md#first-time-setup-ubuntu-2204-native-or-wsl2)
once: host packages, KVM, the free QNX license, Software Center, SDP 8.0.4, an
SSH key, the QEMU image and the Python virtual environment. Then, from the repo root:

```sh
source ~/qnx800/qnxsdp-env.sh
sudo ./scripts/qemu-br0-dhcp.sh                                        # QEMU network, once per boot
(cd targets/qemu-x86_64 && mkqnximage --run=-h && mkqnximage --getip)  # boot QNX
./scripts/run-tests.sh qemu                                            # build, deploy, run on QNX
.venv/bin/arbiter bench run --target qemu                              # fault bench: F1, F4, F5, F6, F9
./scripts/host-checks.sh                                               # host unit tests, analyzers, traceability
```

Expected:

- `run-tests.sh`: `RESULT PASS test_libcontract 64/64`, `RESULT PASS test_contract_file 11/11`,
  `RESULT PASS watchdog_suite`, `RESULT PASS arm_suite`, `RESULT PASS guardian_suite` and
  `SUMMARY 9/9 tests passed on qemu`. The log is saved under `evidence/runs/`.
- `arbiter bench run` (about a minute): one line per check for each scenario, then
  `SUMMARY bench 5/5 scenarios passed on qemu` and the path of `report.html` under
  `evidence/bench/`. Open it: each scenario states its pass condition, lists every check
  with what decided it, and shows a top view of the arm's path next to the operator's hand
  area. On QEMU, F1 measures and prints control-tick spacing but does not judge it; the
  same check is strict on the Pi 5 (see [`docs/TRACEABILITY.md`](docs/TRACEABILITY.md), F1).
- `host-checks.sh`: `SUMMARY host-checks: PASS`.

To compile a contract by hand: `.venv/bin/arbiter contract compile contracts/default.yaml -o build/contract.bin`.

## Where to look

- [`docs/WHY_QNX.md`](docs/WHY_QNX.md): one page, each QNX feature mapped to the failure it handles, the scenario that proves it and its log.
- [`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md): what the attacker controls, what is trusted, what is out of scope, and the claims.
- [`docs/QNX_EVIDENCE.md`](docs/QNX_EVIDENCE.md): every QNX feature used, why, where, its 8.0 doc page, and the log that proves it ran.
- [`docs/TARGETS.md`](docs/TARGETS.md): exact host, SDP and target versions, setup steps, and the WSL2 networking fix.
- [`docs/REQUIREMENTS_SUMMARY.md`](docs/REQUIREMENTS_SUMMARY.md): the requirement IDs cited in code, tests and commits.
- [`docs/TRACEABILITY.md`](docs/TRACEABILITY.md): each requirement, the code that implements it, its tests and where they passed.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): how the guardian decides, units, path checking, and leaving a keep-out zone.
- [`evidence/`](evidence/): saved on-target run logs, bench reports with their raw records, and the host record.
- [`host/arbiter/bench/`](host/arbiter/bench/): the fault bench: one module per scenario, and the independent checks.
- [`qnx/samples/msgpass/msgpass.c`](qnx/samples/msgpass/msgpass.c): the message-passing sample.

QNX SDP files and images built from them are QNX-licensed and are never
committed; the scripts build them on your machine.

## License

Apache-2.0. See [`LICENSE`](LICENSE).
