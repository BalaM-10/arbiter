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

## Status: milestone M1 (safety core), in progress

| Works today, on QNX 8.0 in QEMU | Not built yet |
| --- | --- |
| QNX SDP 8.0.4 toolchain, builds for x86_64 and aarch64le | Watchdog |
| QEMU x86_64 target built with `mkqnximage`, reached over SSH | Evidence logger, gateway |
| Message-passing sample: two processes, send/receive/reply, rejection of malformed messages, pulses, sender identity (7 checks) | Planner, fault bench, dashboard |
| Scripted build, deploy and on-target test run, with every log saved | Raspberry Pi 5 target and timing measurements |
| `libcontract`: the guardian's decision logic (contract table check, command checks in order, keep-out along the whole path, states) as a pure library, 64 tests on the host and on QNX | |
| Contract compiler (`arbiter contract`): strict YAML validation, binary table, checked against the C loader on the host and on QNX | |
| Guardian (`arb-guardian`): resource manager at `/dev/arbiter`, devctl verdicts, message checks, silence and client-loss HOLD, `TimerTimeout`-bounded arm link | |
| Arm controller (`arb-arm`): 1 kHz loop on its own timer, simulated 2-joint arm, its own speed and limit checks, guardian-only sender binding, 10 ms motion lease, brake on guardian loss, read-only telemetry; end to end with the guardian | |

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
./scripts/host-checks.sh                                               # host unit tests, analyzers, traceability
```

Expected: `RESULT PASS test_libcontract 64/64`, `RESULT PASS test_contract_file 11/11`,
`RESULT PASS arm_suite`, `RESULT PASS guardian_suite` and `SUMMARY 8/8 tests passed on qemu`,
then `SUMMARY host-checks: PASS`.

To compile a contract by hand: `.venv/bin/arbiter contract compile contracts/default.yaml -o build/contract.bin`.
The full log is saved under `evidence/runs/`.

## Where to look

- [`docs/WHY_QNX.md`](docs/WHY_QNX.md): one page, each QNX feature mapped to the failure it handles, the scenario that proves it and its log.
- [`docs/THREAT_MODEL.md`](docs/THREAT_MODEL.md): what the attacker controls, what is trusted, what is out of scope, and the claims.
- [`docs/QNX_EVIDENCE.md`](docs/QNX_EVIDENCE.md): every QNX feature used, why, where, its 8.0 doc page, and the log that proves it ran.
- [`docs/TARGETS.md`](docs/TARGETS.md): exact host, SDP and target versions, setup steps, and the WSL2 networking fix.
- [`docs/REQUIREMENTS_SUMMARY.md`](docs/REQUIREMENTS_SUMMARY.md): the requirement IDs cited in code, tests and commits.
- [`docs/TRACEABILITY.md`](docs/TRACEABILITY.md): each requirement, the code that implements it, its tests and where they passed.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): how the guardian decides, units, path checking, and leaving a keep-out zone.
- [`evidence/`](evidence/): saved on-target run logs and the host record.
- [`qnx/samples/msgpass/msgpass.c`](qnx/samples/msgpass/msgpass.c): the message-passing sample.

QNX SDP files and images built from them are QNX-licensed and are never
committed; the scripts build them on your machine.

## License

Apache-2.0. See [`LICENSE`](LICENSE).
