# Arbiter

**The AI proposes. QNX decides.**

Arbiter is a QNX-based runtime safety prototype that validates AI-generated
motion commands against an explicit contract and demonstrates fault containment
for a defined set of failures, with reproducible test evidence.

It runs on **QNX OS 8.0**, between an untrusted AI planner and a (simulated)
robot arm. It is a safety-oriented design built for the QNX Global Hackathon; it
is not certified and not intended for production use.

## Status: milestone M1 (safety core), in progress

| Works today, on QNX 8.0 in QEMU | Not built yet |
| --- | --- |
| QNX SDP 8.0.4 toolchain, builds for x86_64 and aarch64le | Guardian (resource manager at `/dev/arbiter`) |
| QEMU x86_64 target built with `mkqnximage`, reached over SSH | Arm controller, watchdog, evidence logger, gateway |
| Message-passing sample: two processes, send/receive/reply, rejection of malformed messages, pulses, sender identity (7 checks) | Safety contract, planner, fault bench, dashboard |
| Scripted build, deploy and on-target test run, with every log saved | Raspberry Pi 5 target and timing measurements |
| `libcontract`: the guardian's decision logic (contract table check, command checks in order, keep-out along the whole path, states) as a pure library, 58 tests on the host and on QNX | |

## Quick start (QEMU)

Tested with QNX SDP 8.0.4 on Ubuntu 22.04 (WSL2 on Windows 11), QEMU 6.2,
toolchain `qcc` (GCC 12.2.0), target QEMU x86_64. Do the
[first-time setup](docs/TARGETS.md#first-time-setup-ubuntu-2204-native-or-wsl2)
once: host packages, KVM, the free QNX license, Software Center, SDP 8.0.4, an
SSH key and the QEMU image. Then, from the repo root:

```sh
source ~/qnx800/qnxsdp-env.sh
sudo ./scripts/qemu-br0-dhcp.sh                                        # QEMU network, once per boot
(cd targets/qemu-x86_64 && mkqnximage --run=-h && mkqnximage --getip)  # boot QNX
./scripts/run-tests.sh qemu                                            # build, deploy, run on QNX
./scripts/host-checks.sh                                               # host unit tests, analyzers, traceability
```

Expected: `RESULT PASS test_libcontract 58/58` and `SUMMARY 3/3 tests passed on qemu`,
then `SUMMARY host-checks: PASS`.
The full log is saved under `evidence/runs/`.

## Where to look

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
