"""The planner protocol, version 1 (docs/PROTOCOL.md): 64-byte frames, little-endian.

`encode()` turns a frame object into its 64 bytes and `decode()` turns 64 bytes
back into a frame object. Both directions are here, so the same module serves
the planner, observers and test doubles. `decode()` is as strict as the gateway:
wrong magic, version or type, or any non-zero reserved or unused byte, raises
`FrameError` with the protocol's ERROR code.
"""

from __future__ import annotations

import struct
from dataclasses import astuple, dataclass

FRAME_LEN = 64
VERSION = 1
MAGIC = b"ARBP"
PLANNER_PORT = 7700
OBSERVER_PORT = 7701

# VERDICT result
DECIDED, REFUSED, UNAVAILABLE, REJECTED = 0, 1, 2, 3
# ERROR codes
E_MAGIC, E_VERSION, E_TYPE, E_RESERVED, E_ORDER, E_BUSY, E_UNAVAILABLE, E_SLOW, E_READ_ONLY = range(1, 10)

STATES = {1: "HOLD", 2: "NORMAL", 3: "SAFE_STOP"}
VERDICTS = {1: "ALLOW", 2: "CLAMP", 3: "DENY"}
RULES = {0: "NONE", 1: "MALFORMED", 2: "SEQUENCE", 3: "RATE", 4: "JOINT_LIMIT", 5: "VELOCITY",
         6: "KEEPOUT", 7: "STATE", 8: "INTERNAL", 9: "DURATION", 10: "ARM_FAULT", 11: "STALE_START"}


class FrameError(ValueError):
    """A frame the protocol refuses; `code` is the ERROR code the gateway would send."""

    def __init__(self, code: int, why: str) -> None:
        super().__init__(f"{why} (ERROR {code})")
        self.code = code


@dataclass(frozen=True)
class Hello:
    pass


@dataclass(frozen=True)
class Command:
    seq: int
    duration_ms: int
    sender_ts_ns: int
    target_urad: tuple[int, int]


@dataclass(frozen=True)
class Estop:
    pass


@dataclass(frozen=True)
class Welcome:
    state: int
    stop_cause: int
    has_seq: int
    joints: int
    last_seq: int
    session: int
    telemetry_period_ms: int


@dataclass(frozen=True)
class Verdict:
    seq: int
    result: int
    verdict: int
    rule: int
    state: int
    flags: int
    fwd_urad: tuple[int, int]
    fwd_duration_ms: int
    requested_ms: int


@dataclass(frozen=True)
class Telemetry:
    index: int
    t_ns: int
    pos_urad: tuple[int, int]
    vel_urad_s: tuple[int, int]
    segment: int
    arm_state: int
    lease_ok: int
    dropped: int


@dataclass(frozen=True)
class State:
    state: int
    stop_cause: int
    has_seq: int
    last_seq: int
    decisions: int
    estops: int
    watchdog_lost: int
    submits_refused: int
    resets: int


@dataclass(frozen=True)
class Error:
    code: int


Frame = Hello | Command | Estop | Welcome | Verdict | Telemetry | State | Error

# type code, body layout after the 8-byte header (fields in dataclass order,
# with tuples flattened), and the padding bytes that make the reserved fields.
_HDR = struct.Struct("<4sBBH")
_LAYOUT: dict[type, tuple[int, struct.Struct]] = {
    Hello: (0x01, struct.Struct("<")),
    Command: (0x02, struct.Struct("<IIQii")),
    Estop: (0x03, struct.Struct("<")),
    Welcome: (0x81, struct.Struct("<BBBBIII")),
    Verdict: (0x82, struct.Struct("<IBBBB" "B3x" "iiII")),
    Telemetry: (0x83, struct.Struct("<Qqiiii" "IBB2x" "I")),
    State: (0x84, struct.Struct("<BBB1x" "IIHHHH")),
    Error: (0x8F, struct.Struct("<B")),
}
_BY_CODE = {code: (cls, body) for cls, (code, body) in _LAYOUT.items()}


def _flat(frame: Frame) -> list[int]:
    out: list[int] = []
    for v in astuple(frame):
        if isinstance(v, tuple):
            out.extend(v)
        else:
            out.append(v)
    return out


def _unflat(cls: type, values: tuple[int, ...]) -> Frame:
    v = list(values)
    if cls is Command:
        return Command(v[0], v[1], v[2], (v[3], v[4]))
    if cls is Verdict:
        return Verdict(v[0], v[1], v[2], v[3], v[4], v[5], (v[6], v[7]), v[8], v[9])
    if cls is Telemetry:
        return Telemetry(v[0], v[1], (v[2], v[3]), (v[4], v[5]), *v[6:])
    frame: Frame = cls(*v)
    return frame


def encode(frame: Frame) -> bytes:
    """The frame's 64 bytes. Raises ValueError for a value its field cannot hold."""
    code, body = _LAYOUT[type(frame)]
    try:
        payload = body.pack(*_flat(frame))
    except struct.error as err:
        raise ValueError(f"{type(frame).__name__}: {err}") from err
    b = _HDR.pack(MAGIC, VERSION, code, 0) + payload
    return b + bytes(FRAME_LEN - len(b))


def decode(b: bytes) -> Frame:
    """The frame in 64 bytes. ValueError for another length, FrameError for a malformed frame."""
    if len(b) != FRAME_LEN:
        raise ValueError(f"a frame is {FRAME_LEN} bytes, got {len(b)}")
    magic, version, code, reserved = _HDR.unpack_from(b)
    if magic != MAGIC:
        raise FrameError(E_MAGIC, "bad magic")
    if version != VERSION:
        raise FrameError(E_VERSION, f"version {version}")
    if code not in _BY_CODE:
        raise FrameError(E_TYPE, f"unknown type 0x{code:02x}")
    cls, body = _BY_CODE[code]
    values = body.unpack_from(b, _HDR.size)
    if reserved != 0 or any(b[_HDR.size + body.size:]) or encode(_unflat(cls, values)) != b:
        raise FrameError(E_RESERVED, "a reserved or unused byte is not zero")
    return _unflat(cls, values)
