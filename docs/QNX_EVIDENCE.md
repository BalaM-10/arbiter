# QNX evidence map

Every QNX 8.0 feature this repository uses, why it is needed, where it is used,
the 8.0 documentation page, and the saved output that shows it running on QNX.

Status: milestone M0. Only the environment and a message-passing **sample**
exist so far. The sample is a learning and toolchain check, not part of the
Arbiter guardian; rows for the guardian, arm controller and watchdog are added
as those components land.

Target for all proofs below: QEMU x86_64 image built with `mkqnximage`,
QNX SDP 8.0.4 (see `docs/TARGETS.md`).

## Message passing

| QNX feature | Why Arbiter needs it | File | Function | 8.0 doc page | Proof |
| --- | --- | --- | --- | --- | --- |
| Channels (`ChannelCreate`, `ChannelDestroy`) | Every trusted server receives on its own channel; the sample proves the toolchain and target support it before the guardian depends on it | `qnx/samples/msgpass/msgpass.c` | `main()` | [ChannelCreate](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html), [ChannelDestroy](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channeldestroy.html) | `evidence/runs/2026-11-13-qemu/run-090143.log`, M0-MSG-7; 50/50 repeat: `msgpass-repeat50-090153.log` |
| Connections (`ConnectAttach`, `ConnectDetach`) | Clients reach a server's channel only through an explicit connection; fork() does not carry it over, so each process attaches its own | `qnx/samples/msgpass/msgpass.c` | `run_client()` | [ConnectAttach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html), [ConnectDetach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectdetach.html), [fork](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fork.html) | `run-090143.log`, M0-MSG-1 |
| Synchronous send/receive/reply (`MsgSend`, `MsgReceive`, `MsgReply`) | The sender blocks until the server answers, so a command is never acted on before its verdict and a flooding client throttles itself (NFR-Q2) | `qnx/samples/msgpass/msgpass.c` | `send_req()`, `serve()` | [MsgSend](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html), [MsgReceive](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html), [MsgReply](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreply.html) | `run-090143.log`, M0-MSG-1 |
| Rejecting a message (`MsgError`) | Malformed input is refused with an error status and no payload, after a length check made before any field is read (NFR-I6) | `qnx/samples/msgpass/msgpass.c` | `serve()` | [MsgError](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html) | `run-090143.log`, M0-MSG-3, -4, -5 |
| Sender identity (`struct _msg_info`) | The arm controller must accept setpoints only from the guardian (FR-G6); the kernel-filled sender pid is the basis | `qnx/samples/msgpass/msgpass.c` | `serve()` | [_msg_info](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/_msg_info.html) | `run-090143.log`, M0-MSG-2 |
| Pulses (`MsgSendPulse`, `struct _pulse`) | Heartbeats and timer ticks must not block the sender (NFR-Q2) | `qnx/samples/msgpass/msgpass.c`, `qnx/common/arb_pulses.h` | `run_client()`, `serve()` | [MsgSendPulse](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html), [_pulse](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/_pulse.html) | `run-090143.log`, M0-MSG-6 |

## Resource manager

None yet (M1: guardian at `/dev/arbiter`).

## Real-time services

None yet (M1: arm control loop timer, FIFO priorities, cycle counter).

## Security policies

None yet (M2). Present in the SDP: `secpolcompile`, `secpolpush`, `secpol`,
`secpolgenerate` (see `docs/TARGETS.md`).

## Microkernel isolation

None yet beyond the sample's two separate processes (M1 onward).

## Tracing and logging

None yet (M1: `slog2`; M2: kernel trace).

## Image and boot

| QNX feature | Why Arbiter needs it | File | Function | 8.0 doc page | Proof |
| --- | --- | --- | --- | --- | --- |
| `mkqnximage` QEMU image | A reproducible x86_64 QNX target that judges can build without hardware (NFR-K3) | `docs/TARGETS.md`, `scripts/qemu-br0-dhcp.sh` | build and run steps | [mkqnximage](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html) | `evidence/runs/2026-11-13-qemu/run-090143.log` header: `uname -a` on the target |
| `qcc` cross-compiler | Every QNX binary is built with the SDP 8.0 toolchain for x86_64 and aarch64le (NFR-K1) | `qnx/Makefile` | `all` | [qcc](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/q/qcc.html) | `evidence/runs/2026-11-13-qemu/hello.log` |
