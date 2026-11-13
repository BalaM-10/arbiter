# QNX evidence map

Every QNX 8.0 feature this repository uses, why it is needed, where it is used,
the 8.0 documentation page, and the saved output that shows it running on QNX.

Status: milestone M1, in progress. The guardian (`qnx/guardian/`) runs on QNX,
tested against a stand-in arm (`qnx/tests/arb_arm_stub.c`); the arm controller
and the watchdog are added as they land. Rows marked "sample" come from the M0
message-passing sample, a toolchain check that is not part of the guardian.
Why each feature matters in the scenario: `docs/WHY_QNX.md`.

Target for all proofs below: QEMU x86_64 image built with `mkqnximage`,
QNX SDP 8.0.4 (see `docs/TARGETS.md`). Requirement IDs in parentheses are
defined in `docs/REQUIREMENTS_SUMMARY.md`.

## Message passing

| QNX feature | Why Arbiter needs it | File | Function | 8.0 doc page | Proof |
| --- | --- | --- | --- | --- | --- |
| Channels (`ChannelCreate`, `ChannelDestroy`) | Every trusted server receives on its own channel; the sample proves the toolchain and target support it before the guardian depends on it | `qnx/samples/msgpass/msgpass.c` | `main()` | [ChannelCreate](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html), [ChannelDestroy](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channeldestroy.html) | `evidence/runs/2026-11-13-qemu/run-102030.log` (`REPEAT=50`, 50/50 passed), M0-MSG-7 |
| Connections (`ConnectAttach`, `ConnectDetach`) | Clients reach a server's channel only through an explicit connection; fork() does not carry it over, so each process attaches its own | `qnx/samples/msgpass/msgpass.c` | `run_client()` | [ConnectAttach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html), [ConnectDetach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectdetach.html), [fork](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fork.html) | `run-102030.log`, M0-MSG-1 |
| Synchronous send/receive/reply (`MsgSend`, `MsgReceive`, `MsgReply`) | The sender blocks until the server answers, so a command is never acted on before its verdict and a flooding client throttles itself (NFR-Q2) | `qnx/samples/msgpass/msgpass.c` | `send_req()`, `serve()` | [MsgSend](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html), [MsgReceive](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html), [MsgReply](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreply.html) | `run-102030.log`, M0-MSG-1 |
| Rejecting a message (`MsgError`) | Malformed input is refused with an error status and no payload, after a length check made before any field is read (NFR-I6) | `qnx/samples/msgpass/msgpass.c` | `serve()` | [MsgError](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html) | `run-102030.log`, M0-MSG-3, -4, -5 |
| Sender identity (`struct _msg_info`) | The arm controller must accept setpoints only from the guardian (FR-G6). The sample shows the kernel, not the sender, reports who is calling. A pid alone is not enough (pids are reused), so the arm will also use a private or policy-typed channel and per-connection identity (M1/M2) | `qnx/samples/msgpass/msgpass.c` | `serve()` | [_msg_info](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/_msg_info.html) | `run-102030.log`, M0-MSG-2 |
| Pulses (`MsgSendPulse`, `struct _pulse`) | Heartbeats and timer ticks must not block the sender (NFR-Q2) | `qnx/samples/msgpass/msgpass.c`, `qnx/common/arb_pulses.h` | `run_client()`, `serve()` | [MsgSendPulse](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html), [_pulse](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/_pulse.html) | `run-102030.log`, M0-MSG-6 |

## Resource manager

| QNX feature | Why Arbiter needs it | File | Function | 8.0 doc page | Proof |
| --- | --- | --- | --- | --- | --- |
| Resource manager registered at `/dev/arbiter` (`resmgr_attach`, `iofunc_func_init`, `iofunc_attr_init`) | The guardian is the one named, permission-checked entry point for commands (FR-G1, NFR-Q3) | `qnx/guardian/main.c` | `main()` | [resmgr_attach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/resmgr_attach.html), [skeleton](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/skeleton_SIMPLE_ST_EG.html) | pending (run after commit): `guardian_suite` (`ls -l /dev/arbiter`, `nfr_q3_*`) |
| `devctl()` handling with message checks (`iofunc_devctl_default`, `_IO_INPUT_PAYLOAD`, `_RESMGR_PTR`) | Each command arrives as a devctl carried by native message passing; sizes are taken from `ctp->size` and `srcmsglen`, never from the client's `nbytes` (FR-G1, NFR-I6) | `qnx/guardian/main.c` | `io_devctl()` | [devctl handling](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.resmgr/topic/messages_HANDLING_devctl.html), [message checking](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/write_resource_manager.html) | pending (run after commit): `test_guardian normal` (`fr_g1_*`, `nfr_i6_*`) |
| Close handler (`close_ocb`, `iofunc_close_ocb_default`) | A dead or disconnected client is noticed at once: the guardian moves to HOLD without waiting for the silence timeout (FR-G5, TM-C4) | `qnx/guardian/main.c` | `io_close_ocb()` | [client termination](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_Client_termination.html), [iofunc_close_ocb_default](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_close_ocb_default.html) | pending (run after commit): `fr_g5_last_client_close_enters_hold` |
| Dispatch layer on our own channel (`dispatch_create_channel`, `pulse_attach`, `dispatch_block`, `dispatch_handler`) | The channel's flags are ours (`_NTO_CHF_FIXED_PRIORITY`), and the tick pulse shares the guardian's single receive loop | `qnx/guardian/main.c` | `main()`, `on_tick()` | [dispatch_create_channel](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create_channel.html), [pulse_attach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pulse_attach.html) | pending (run after commit): `guardian_suite` |

## Real-time services

| QNX feature | Why Arbiter needs it | File | Function | 8.0 doc page | Proof |
| --- | --- | --- | --- | --- | --- |
| FIFO priority in the privileged range (`pthread_setschedparam`, `SCHED_FIFO`) | Trusted threads sit above anything an untrusted process can use, so none can starve or boost them (NFR-I3, TM-C3) | `qnx/common/arb_prio.h`, `qnx/guardian/main.c` | `main()` | [pthread_setschedparam](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedparam.html), [on](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/o/on.html) | pending (run after commit): `nfr_i3_*` (`pidin` shows `64f`) |
| Periodic timer delivering a pulse (`timer_create`, `timer_settime`, `SIGEV_PULSE_INIT`) | The guardian's 1 ms tick runs its silence timeout on its own clock (FR-G5) | `qnx/guardian/main.c` | `main()`, `on_tick()` | [timer_create](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html), [sigevent](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigevent.html) | pending (run after commit): `fr_g5_silence_enters_hold_over_devctl` |
| Bounded send (`TimerTimeout` with `_NTO_TIMEOUT_SEND \| _NTO_TIMEOUT_REPLY`) | A hung or stopped arm cannot hang the guardian; the send times out and the guardian enters SAFE_STOP (FR-G9, TM-C4) | `qnx/guardian/arm_link.c`; `qnx/tests/test_send_timeout.c` | `bounded_send()` | [TimerTimeout](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html) | `evidence/runs/2026-11-13-qemu/run-133305.log` (`test_send_timeout`, 2 ms requested, 2 to 4 ms measured); guardian: pending (run after commit) (`fr_g9_*`) |
| Clock period and cycle counter (`ClockPeriod`, `clock_getres`, `ClockCycles`) | Timing is measured, not assumed: the tick decides how a 2 ms timeout behaves (NFR-Q4 groundwork) | `qnx/tests/test_send_timeout.c` | `main()` | [ClockPeriod](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockperiod.html), [ClockCycles](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clockcycles.html) | `run-133305.log` (tick 1000000 ns) |

## Security policies

None yet (M2). Present in the SDP: `secpolcompile`, `secpolpush`, `secpol`,
`secpolgenerate` (see `docs/TARGETS.md`).

## Microkernel isolation

| QNX feature | Why Arbiter needs it | File | Function | 8.0 doc page | Proof |
| --- | --- | --- | --- | --- | --- |
| Internal service name (`name_open`; stand-in arm: `name_attach` on its own channel) | The guardian reaches the arm only through an internal name; the arm's channel keeps the flags it chose (no `_NTO_CHF_UNBLOCK`) (FR-G6, FR-G9) | `qnx/guardian/arm_link.c`; `qnx/tests/arb_arm_stub.c` | `arm_link_open()`; `main()` | [name_open](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html), [name_attach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html) | pending (run after commit): `fr_g6_name_attach_uses_own_channel` |
| Separate processes for guardian, arm and clients | A client's malformed input or death cannot corrupt the guardian (TM-C4, TM-C8) | `qnx/tests/guardian_suite.sh` | — | — | pending (run after commit): `nfr_i6_random_payloads_never_crash_the_guardian` |

## Tracing and logging

None yet (M1: `slog2`; M2: kernel trace).

## Image and boot

| QNX feature | Why Arbiter needs it | File | Function | 8.0 doc page | Proof |
| --- | --- | --- | --- | --- | --- |
| `mkqnximage` QEMU image | A reproducible x86_64 QNX target that judges can build without hardware (NFR-K3) | `docs/TARGETS.md`, `scripts/qemu-br0-dhcp.sh` | build and run steps | [mkqnximage](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html) | `evidence/runs/2026-11-13-qemu/run-102030.log`, target section: `uname -a`; versions in `evidence/host/2026-11-13-host.txt` |
| `qcc` cross-compiler | Every QNX binary is built with the SDP 8.0 toolchain for x86_64 and aarch64le (NFR-K1) | `qnx/Makefile` | `all` | [qcc](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/q/qcc.html) | `evidence/runs/2026-11-13-qemu/run-102030.log`, build section: both architectures, `file` type and SHA-256 per binary |
