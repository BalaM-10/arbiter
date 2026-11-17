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

## Status: milestone M2 (untrusted side), in progress

| Works today, on QNX 8.0 in QEMU | Not built yet |
| --- | --- |
| QNX SDP 8.0.4 toolchain, builds for x86_64 and aarch64le | |
| QEMU x86_64 target built with `mkqnximage`, reached over SSH | Fault scenarios F2, F3, F7, F8, F10, F12 to F15 in the bench (M2 P7) |
| Scripted build, deploy and on-target test run, with every log saved | Raspberry Pi 5 target, timing measurements, dashboard (M3) |
| `libcontract`: the guardian's decision logic (contract table check, command checks in order, keep-out along the whole path, states) as a pure library, 72 tests on the host and on QNX; keep-out checks both links of the arm as segments | |
| Contract compiler (`arbiter contract`): strict YAML validation, binary table, checked against the C loader on the host and on QNX | |
| Guardian (`arb-guardian`): resource manager at `/dev/arbiter`, devctl verdicts, message checks, silence and client-loss HOLD, `TimerTimeout`-bounded arm link; commands only from the gateway's user, one submitter at a time, HOLD at once when it goes, last sequence number published | |
| Arm controller (`arb-arm`): 1 kHz loop on its own timer, simulated 2-joint arm, its own speed and limit checks, guardian-only sender binding, 10 ms motion lease, brake on guardian loss, read-only telemetry; end to end with the guardian | |
| Watchdog (`arb-watchdog`): kernel death notices and heartbeats for guardian and arm; brakes the arm or stops the guardian through registered events; records which failure occurred. The arm and the guardian accept its beat and trip only on the connection they registered with, so a forged pulse with the right value is refused | |
| **Fault bench** (`arbiter bench run`): F1 normal pick-and-place, F4 joint limit, F5 keep-out, F6 over-speed, F9 replay, sent from the host through the planner protocol and the gateway, run against the live target and judged from the arm's own telemetry with independent Python kinematics, not from the guardian's verdicts. [Latest report: 5/5](evidence/bench/2026-11-17-qemu/135730/report.html) | |
| Watchdog failure is fail-safe (M2): if the watchdog dies or freezes, the arm brakes and latches and the guardian enters SAFE_STOP, each on its own; a watchdog that comes back restarts nothing | |
| Emergency stop and operator reset (M2): `arb-ctl estop` from any client stops everything; `arb-ctl reset` works only for the operator's user ID, and only once a watchdog is running | |
| Isolation without the security policy (M2): every process runs as its own user; the trusted ones drop root and keep only the abilities they need, and check at startup that the rest is refused; the arm accepts only the guardian's user, and the guardian and the arm check who serves a name before trusting it. F11 (bypass, TM-C2) passes every case with the policy off, including an untrusted user submitting commands | |
| Kernel security policy (M2, QEMU): a second image enforces `qnx/image/policy/arbiter.secpol`, pushed at boot: each Arbiter process has its own type, only the guardian's type may connect to the arm or attach `/dev/arbiter`, and F11's bypass attempts are refused in the kernel before any Arbiter code runs (TM-C2). The rules are read back from the kernel with `secpol` into the run log | |
| System log (M2): guardian, arm and watchdog record startup, state changes, brakes, refused senders and watchdog trips through the QNX system logger, read on the target with `slog2info -b arb_guardian` (or `arb_arm`, `arb_watchdog`) | |
| Evidence log (M2 P6): the guardian records every decision with its latency in a ring only the logger may read; `arb-logd` writes a SHA-256 hash-chained log (QNX Cryptography Library) and declares anything it lost; `arbiter evidence verify` recomputes the chain on the host and names the first edited, removed or reordered line. Every bench scenario verifies the target's log. Format: [`docs/EVIDENCE_LOG.md`](docs/EVIDENCE_LOG.md) | |
| Gateway (`arb-gateway`, M2 P6): a planner connects over TCP and speaks the 64-byte frames of [`docs/PROTOCOL.md`](docs/PROTOCOL.md); each command is passed to the guardian as the gateway's user and its verdict comes back, with read-only observers getting a copy, and the arm's joint state about every 10 ms, relayed by the evidence logger so the gateway maps no trusted memory. One planner at a time; any malformed frame ends the session and is never forwarded; when the planner leaves the guardian holds at once, and a reconnecting planner continues at the guardian's last sequence number + 1. The gateway runs below every trusted process as its own user (and type, on the policy image), listens on the target's wired interface only (never every address), and after its startup the network stack refuses it any new socket | |
| Scripted planner (`arbiter planner`, M2 P6): plays the fixed pick-and-place path from the host to the gateway, resuming at the guardian's last sequence number + 1. It is never given the contract and reads no key; `arbiter planner e2e` runs it end to end against QNX and checks the target's evidence log | |
| Evidence packet (FR-E3): every bench run writes `packet.json` next to its report: the contract hash, each binary's SHA-256 as the target hashes it and as built, the target and its security policy, the results, every scenario's hash-chained log with its anchor, and every file's hash; `arbiter evidence verify-packet <folder>` checks it all | |
| Message-passing sample from M0: send/receive/reply, malformed messages, pulses, sender identity (7 checks) | |

On QEMU every result proves function only, never timing; timing claims will
come only from the Raspberry Pi 5 (M3).

**On QEMU, a host pause over 10 ms makes Arbiter safe-stop, by design.** The
host can pause the virtual machine (WSL2 or Windows scheduling, a busy laptop).
Inside the guest every process then looks silent for that long, so the watchdog
trips, the arm brakes and latches and the guardian enters `SAFE_STOP`. Nothing
moves until the operator resets. On the target, from the deploy directory:
`cp arb-ctl /tmp/ && on -u 800 /tmp/arb-ctl reset` (user 800 is the operator and
has no access to root's home; root itself is refused), then `./arb-ctl status` shows
`HOLD` and the next command moves the arm again.
Measured: pauses of 20 to 200 ms each stopped the system and each was recovered
by that reset; a 9 ms pause stopped nothing
([`evidence/stall/2026-11-16-qemu/210332-pause.log`](evidence/stall/2026-11-16-qemu/210332-pause.log)).
How often it happens on its own: [`docs/TARGETS.md`](docs/TARGETS.md#qemu-host-pauses).
Which stops need the operator: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Quick start (QEMU)

Tested with QNX SDP 8.0.4 on Ubuntu 22.04 (WSL2 on Windows 11), QEMU 6.2,
toolchain `qcc` (GCC 12.2.0), target QEMU x86_64. Do the
[first-time setup](docs/TARGETS.md#first-time-setup-ubuntu-2204-native-or-wsl2)
once: host packages, KVM, the free QNX license, Software Center, SDP 8.0.4, an
SSH key, the QEMU image and the Python virtual environment. There are two QEMU images: one without the kernel security policy, where every
test runs, and one that enforces it, to show the same attacks refused in the
kernel as well (FR-S6). Then, from the repo root:

```sh
source ~/qnx800/qnxsdp-env.sh
sudo ./scripts/qemu-br0-dhcp.sh                                        # QEMU network, once per boot
(cd targets/qemu-x86_64 && mkqnximage --run=-h && mkqnximage --getip)  # boot QNX
./scripts/run-tests.sh qemu                                            # build, deploy, run on QNX
.venv/bin/arbiter bench run --target qemu                              # fault bench: F1, F4, F5, F6, F9
.venv/bin/arbiter planner e2e --target qemu                            # the scripted planner, through the gateway
./scripts/host-checks.sh                                               # host unit tests, analyzers, traceability
```

Expected:

- `run-tests.sh`: `RESULT PASS test_libcontract 72/72`, `RESULT PASS test_contract_file 11/11`,
  `RESULT PASS watchdog_suite`, `RESULT PASS arm_suite`, `RESULT PASS guardian_suite` and
  `SUMMARY 11/11 tests passed on qemu`. The log is saved under `evidence/runs/`.
- `arbiter bench run` (about a minute): one line per check for each scenario, then
  `SUMMARY bench 5/5 scenarios passed on qemu` and the path of `report.html` under
  `evidence/bench/`, then `packet: PASS ... packet.json` (the evidence packet, checked). Open it: each scenario states its pass condition, lists every check
  with what decided it, and shows a top view of the arm's path next to the operator's hand
  area. On QEMU, F1 measures and prints control-tick spacing but does not judge it; the
  same check is strict on the Pi 5 (see [`docs/TRACEABILITY.md`](docs/TRACEABILITY.md), F1).
- `arbiter planner e2e` (about 30 s): the planner on this host plays the pick-and-place path
  over TCP to the gateway on QNX, one `C seq ... -> ALLOW NONE` line per command, then the
  target's evidence log is checked and it ends with
  `PASS fr_p1_scripted_planner_runs_pick_and_place_through_the_gateway (qemu)`. To drive a
  running gateway yourself: `.venv/bin/arbiter planner run --host <target address>`.
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
- [`evidence/`](evidence/): saved on-target run logs, bench reports with their raw records and each scenario's system log, the QEMU host-pause measurements ([`evidence/stall/`](evidence/stall/README.md)), and the host record.
- [`host/arbiter/bench/`](host/arbiter/bench/): the fault bench: one module per scenario, and the independent checks.
- [`qnx/samples/msgpass/msgpass.c`](qnx/samples/msgpass/msgpass.c): the message-passing sample.

QNX SDP files and images built from them are QNX-licensed and are never
committed; the scripts build them on your machine.

## License

Apache-2.0. See [`LICENSE`](LICENSE).
