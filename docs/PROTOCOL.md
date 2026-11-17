# Planner protocol (version 1)

How a planner talks to Arbiter's gateway `arb-gateway` (FR-N1, FR-N2, FR-N4).
The gateway is untrusted. It checks that each frame is well formed and passes
commands on to the guardian, which decides. The gateway judges no command: it
does not know the contract, and nothing in this protocol carries a limit
(FR-P3).

The protocol only describes bytes on an ordered stream. It does not depend on
the planner's language, host or kind (scripted or LLM): the scripted planner
on the development host, an LLM planner on another machine, and the bench all
send the same frames. The gateway sees frames, never which planner sent them.

## Transport

- Any reliable, ordered byte stream. Version 1 uses TCP:
  - **port 7700: the planner.** One connection at a time.
  - **port 7701: observers** (dashboard relay, recorders). Up to 2 at a time.
    They only receive.
- **One listening address, never all of them.** By default the gateway listens
  on the target's own wired interface: the first interface that is up and
  running, is not loopback, and has an IPv4 address (on QEMU, the private
  bridge to the development host). `arb-gateway -a <IPv4 address>` or
  `-i <interface>` chooses another; the wildcard address `0.0.0.0` is refused
  and the gateway does not start. Its ready line names the address:
  `arb-gateway: ready, listening on <address> ports 7700 7701 ...`.
- No checksum: the stream is assumed reliable (TCP has its own). A planner on
  another machine reaches the target over the network. Version 1 has no
  authentication or encryption: whoever reaches the port is treated as the
  planner, and is bound by the contract and the guardian like any planner
  (THREAT_MODEL A2, TM-C1, TM-C2). Taking the single planner slot so that the
  real planner is refused is an accepted availability limit (THREAT_MODEL O5).
- No keep-alive. If the planner goes quiet, the guardian moves to `HOLD` after
  the contract's silence timeout. If its connection closes, the gateway closes
  its submit handle and the guardian moves to `HOLD` at once (FR-G10).

## Frames

Every frame, in both directions, is **64 bytes**. Integers are little-endian.
Angles are microradians (µrad), times are milliseconds or nanoseconds as named.

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| 0 | 4 | magic | the bytes `41 52 42 50` ("ARBP") |
| 4 | 1 | version | 1 |
| 5 | 1 | type | see below |
| 6 | 2 | reserved | 0 |
| 8 | 56 | body | by type. Every byte not named in the type's table is 0 |

A receiver rejects any frame with a wrong magic, version or type, a non-zero
reserved byte, or a non-zero unused body byte. Strict zeros keep room for
later versions: a field added later is never mistaken for padding.

### Planner to gateway

| Type | Name | Body |
| --- | --- | --- |
| `0x01` | HELLO | none (all zero) |
| `0x02` | COMMAND | below |
| `0x03` | ESTOP | none (all zero) |

COMMAND (FR-G2):

| Offset | Size | Field |
| --- | --- | --- |
| 8 | 4 | `seq`, unsigned: newer than the last sequence number the guardian consumed |
| 12 | 4 | `duration_ms`, unsigned: requested time to reach the targets |
| 16 | 8 | `sender_ts_ns`, unsigned: the planner's own clock; logged, never trusted |
| 24 | 4 | `target_urad[0]`, signed: shoulder |
| 28 | 4 | `target_urad[1]`, signed: elbow |

The gateway copies these five fields into the guardian's command structure
(`arb_cmd_t`, `qnx/common/arb_proto.h`) unchanged. Out-of-range values are
the guardian's to deny, not the gateway's.

ESTOP: emergency stop (FR-W4). The guardian enters `SAFE_STOP` and the arm
brakes. Any client may stop the arm (decision D2). There is no reset frame:
reset is for the local operator only and is never possible through the gateway.

### Gateway to client

| Type | Name | Sent |
| --- | --- | --- |
| `0x81` | WELCOME | to the planner in answer to HELLO; to an observer as soon as it connects |
| `0x82` | VERDICT | one per COMMAND, in order; observers get a copy |
| `0x83` | TELEMETRY | arm joint state, about every 10 ms (FR-N4) |
| `0x84` | STATE | right after WELCOME, when the guardian's state or stop cause changes, and after an ESTOP |
| `0x8F` | ERROR | just before the gateway closes the connection |

WELCOME:

| Offset | Size | Field |
| --- | --- | --- |
| 8 | 1 | `state`: 1 `HOLD`, 2 `NORMAL`, 3 `SAFE_STOP` |
| 9 | 1 | `stop_cause`: 0 none, 1 violations, 2 arm timeout, 3 watchdog, 4 e-stop |
| 10 | 1 | `has_seq`: 1 if the guardian has consumed a sequence number |
| 11 | 1 | `joints`: 2 |
| 12 | 4 | `last_seq`: the newest sequence number the guardian consumed (if `has_seq`) |
| 16 | 4 | `session`: planner connections accepted since the gateway started (0 on the observer port) |
| 20 | 4 | `telemetry_period_ms`: 10 |

VERDICT (FR-G3):

| Offset | Size | Field |
| --- | --- | --- |
| 8 | 4 | `seq`: echoed from the COMMAND |
| 12 | 1 | `result`: 0 decided by the guardian; 1 refused by the guardian before any decision (another submitter holds it, or the gateway is not running as its own user); 2 guardian unavailable; 3 the guardian rejected the message as malformed |
| 13 | 1 | `verdict`: 1 `ALLOW`, 2 `CLAMP`, 3 `DENY` (0 unless `result` is 0) |
| 14 | 1 | `rule`: the rule that decided (table below) |
| 15 | 1 | `state`: the guardian's state after this decision |
| 16 | 1 | `flags`: bit 0, a keep-out exit move |
| 20 | 4 | `fwd_urad[0]`: shoulder target sent to the arm (`ALLOW`, `CLAMP`), else 0 |
| 24 | 4 | `fwd_urad[1]`: elbow target sent to the arm, else 0 |
| 28 | 4 | `fwd_duration_ms`: duration sent to the arm (stretched on `CLAMP`), else 0 |
| 32 | 4 | `requested_ms`: the duration as requested |

Rules: 0 none (`ALLOW`), 1 malformed, 2 sequence, 3 rate, 4 joint limit,
5 velocity (`CLAMP`), 6 keep-out, 7 state (`SAFE_STOP`), 8 internal, 9 duration,
10 arm fault, 11 stale start (`qnx/libcontract/arb_contract.h`).

If `result` is 2 the gateway then sends ERROR 7 and closes: the planner
reconnects and resumes from the new WELCOME.

TELEMETRY (FR-A6, FR-N4): the arm's newest sample, about every 10 ms. The arm
writes one sample per 1 ms control cycle, so consecutive frames' `index`
differs by about 10; a larger step means the gateway was late. The gateway
maps no trusted memory: the evidence logger reads the arm's ring and answers
the gateway by message passing (decision D1, NFR-I1). Without the logger
there is no TELEMETRY, and commands work as before.

| Offset | Size | Field |
| --- | --- | --- |
| 8 | 8 | `index`: the sample's number in the arm's ring (one per 1 ms control cycle) |
| 16 | 8 | `t_ns`, signed: the arm's monotonic time for this position |
| 24 | 4 | `pos_urad[0]`, signed |
| 28 | 4 | `pos_urad[1]`, signed |
| 32 | 4 | `vel_urad_s[0]`, signed |
| 36 | 4 | `vel_urad_s[1]`, signed |
| 40 | 4 | `segment`: increments with every setpoint the arm accepted |
| 44 | 1 | `arm_state`: 1 idle, 2 moving, 3 stopping, 4 braked |
| 45 | 1 | `lease_ok`: 1 while the arm's motion lease is valid |
| 48 | 4 | `dropped`: TELEMETRY frames this client missed since the last one it got, because its send queue was full |

Telemetry is a view for planners and dashboards. The fault bench does not
judge from it: the bench reads the arm's own ring on the target.

STATE:

| Offset | Size | Field |
| --- | --- | --- |
| 8 | 1 | `state` |
| 9 | 1 | `stop_cause` |
| 10 | 1 | `has_seq` |
| 12 | 4 | `last_seq` |
| 16 | 4 | `decisions`: commands the guardian has decided |
| 20 | 2 | `estops` |
| 22 | 2 | `watchdog_lost` |
| 24 | 2 | `submits_refused` |
| 26 | 2 | `resets` |

ERROR:

| Offset | Size | Field |
| --- | --- | --- |
| 8 | 1 | `code`: 1 bad magic, 2 bad version, 3 bad type, 4 non-zero reserved or unused byte, 5 out of order (COMMAND or ESTOP before HELLO, or a second HELLO), 6 busy (a planner is already connected, or the observer slots are full), 7 guardian unavailable, 8 too slow (the client did not read its frames), 9 read only (an observer sent data) |

## Session

1. The planner connects to port 7700. If a planner is already connected, it
   gets ERROR 6 and the connection closes.
2. The planner sends HELLO. The gateway opens its submit handle on
   `/dev/arbiter` and answers WELCOME, then STATE.
3. **Resume rule.** If `has_seq` is 1, the planner's first COMMAND uses
   `seq = last_seq + 1` (modulo 2^32). If it is 0, any `seq` is accepted. The
   guardian keeps one sequence space for its whole lifetime, so a planner that
   restarts, or a different planner, continues from the WELCOME it gets
   (FR-G10).
4. Each COMMAND gets exactly one VERDICT, in order. The planner may send the
   next COMMAND before the VERDICT arrives, but the guardian's rate limit
   counts every one.
5. The planner leaves by closing the connection. The gateway closes its submit
   handle, and the guardian moves from `NORMAL` to `HOLD` at once.

Any malformed frame ends the session: ERROR, then close. The gateway never
tries to resynchronise inside a stream. One stray byte shifts every later
frame, and a planner that sends one has lost track of its own output.

On the observer port, the gateway sends WELCOME as soon as the observer
connects, then TELEMETRY, STATE and a copy of every VERDICT. An observer that
sends any byte gets ERROR 9.

## Back-pressure

The gateway never blocks on a client. Each connection has a send queue of 64
frames. If the queue is full, a TELEMETRY frame is dropped and counted in the
next one's `dropped`. If any other frame does not fit, the client is too slow:
ERROR 8 (if it fits) and close. For the planner, that means `HOLD`.

## Examples

Byte-exact frames, checked by the host tests (`host/tests/test_protocol.py`)
and by the gateway's on-target test (`qnx/tests/test_gateway.c`).

HELLO:

```text
41 52 42 50 01 01 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

COMMAND `seq` 7, `duration_ms` 1500, `sender_ts_ns` 1000000000, targets
-523599 µrad (-30°) and 1047198 µrad (60°):

```text
41 52 42 50 01 02 00 00 07 00 00 00 dc 05 00 00
00 ca 9a 3b 00 00 00 00 b1 02 f8 ff 9e fa 0f 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

WELCOME: `NORMAL`, no stop cause, `has_seq` 1, 2 joints, `last_seq` 6,
session 1, telemetry every 10 ms:

```text
41 52 42 50 01 81 00 00 02 00 01 02 06 00 00 00
01 00 00 00 0a 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

VERDICT for that command: decided, `CLAMP` by rule 5 (velocity), state
`NORMAL`, forwarded with the duration stretched to 2094 ms:

```text
41 52 42 50 01 82 00 00 07 00 00 00 00 02 05 02
00 00 00 00 b1 02 f8 ff 9e fa 0f 00 2e 08 00 00
dc 05 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

## Versioning

A change to any frame's layout or meaning raises `version`. A gateway answers
a frame of another version with ERROR 2 and closes, so a planner and a gateway
of different versions fail at once instead of misreading each other.
