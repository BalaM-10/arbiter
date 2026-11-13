# Why QNX

Each QNX OS 8.0 feature Arbiter uses, the failure it handles in the tabletop
scenario, the fault scenario that proves it, and the saved log. A row without a
log is designed but not yet proven on QNX. Claims refer to
[`THREAT_MODEL.md`](THREAT_MODEL.md); code locations and 8.0 documentation pages
are in [`QNX_EVIDENCE.md`](QNX_EVIDENCE.md).

| QNX feature | Failure it handles | Proven by | Log |
| --- | --- | --- | --- |
| Synchronous message passing (`MsgSend`/`MsgReceive`/`MsgReply`) | A command is never acted on before its verdict, and a flooding sender is blocked by its own sends (TM-C1, TM-C3) | F3, F8 | Mechanism on QNX: `evidence/runs/2026-11-13-qemu/run-130755.log` (M0-MSG-1, -3, -4); F3, F8: pending (M2) |
| Kernel-reported sender identity (`_msg_info`, `ConnectClientInfo()`) | A process that is not the guardian sends setpoints to the arm (TM-C2) | F11, policy off | Mechanism on QNX: `run-130755.log` (M0-MSG-2); F11: pending (M2) |
| Resource manager at `/dev/arbiter`, close handler | The gateway dies; the guardian learns at once instead of waiting for silence (TM-C4) | F10 | Mechanism on QNX: `fr_g5_last_client_close_enters_hold` in `guardian_suite` (run log of the guardian commit); F10 in M2 |
| Pulses | Heartbeats and lease renewals must never block the sender (TM-C4) | F13 | Mechanism on QNX: `run-130755.log` (M0-MSG-6); F13: pending |
| Strict fixed-priority FIFO scheduling, trusted threads in the privileged range | A flood, a pulse flood or a CPU hog at the gateway's priority delays the arm's control loop or the watchdog, or boosts the guardian above them (TM-C3) | F3, F12 | pending (M1 day 7, M2) |
| `_NTO_CHF_FIXED_PRIORITY` channels | The guardian would otherwise drop to its low-priority client's priority while serving it (TM-C3) | F12 | pending |
| Timer that delivers a pulse (`timer_create`) | The arm's 1 kHz loop and its motion lease run on the arm's own clock, independent of any other process (TM-C4) | F1, F13 | pending (M1 day 4-5) |
| `TimerTimeout()` on the setpoint send | A hung arm controller would hang the guardian; instead the send times out and the guardian enters `SAFE_STOP` (TM-C4) | `fr_g9_arm_never_replies_times_out_and_safe_stops`, `fr_g9_arm_not_receiving_times_out_and_safe_stops` | `test_send_timeout` in `evidence/runs/2026-11-13-qemu/run-133305.log` (2 ms requested, 2 to 4 ms measured); guardian with a stand-in arm: `guardian_suite` (run log of the guardian commit) |
| `_NTO_CHF_DISCONNECT` on the arm's channel | The guardian dies mid-move; the kernel tells the arm, which brakes at once (TM-C4) | F13 | pending |
| Process-manager death notices (`procmgr_event_notify_add`, `MsgRegisterEvent`) | A trusted process dies; the watchdog learns which one and records it (TM-C4) | F13 | pending (M1 day 6) |
| Separate processes on a microkernel | A crash in the gateway or planner cannot corrupt the guardian, the arm or the watchdog (TM-C4) | F10 | pending |
| Security policy (`secpolcompile`, `secpolpush`, `on -T`) | An untrusted process connects to the arm at all; the kernel refuses it before any Arbiter code runs (TM-C2) | F11, policy on | pending (M2) |
| Abilities and privilege dropping (`procmgr_ability`) | A compromised trusted process keeps powers it never needed (NFR-I5) | F11 | pending (M2) |
| Cycle counter (`ClockCycles()`) | Decision latency would be guessed instead of measured (NFR-T2) | Timing runs on the Pi 5 | pending (M3) |
| Kernel trace (`tracelogger`) | The isolation claim would rest on assertion instead of a recording (NFR-Q5) | F3, F12 | pending (M2) |

QEMU runs prove function only. Timing is reported only from Raspberry Pi 5 runs,
as measurements, never as guarantees.
