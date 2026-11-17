"""Host tests of the planner protocol's Python side (FR-N2), `arbiter.protocol`.

The example frames are read from docs/PROTOCOL.md itself, so the document, this
encoder and the gateway's C layout (checked on QNX by
qnx/tests/test_gateway.c::fr_n2_frames_match_the_documented_examples) agree byte
for byte.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from arbiter import protocol as p

DOC = Path(__file__).resolve().parents[2] / "docs" / "PROTOCOL.md"


def _examples() -> dict[str, bytes]:
    """The four example blocks of PROTOCOL.md, keyed by the frame name that introduces each."""
    text = DOC.read_text(encoding="utf-8").split("## Examples", 1)[1].split("\n## ", 1)[0]
    out: dict[str, bytes] = {}
    caption = ""
    for para in text.split("\n\n"):
        para = para.strip()
        m = re.fullmatch(r"```text\n(.*)\n```", para, re.DOTALL)
        if m:
            out[caption] = bytes.fromhex(m.group(1).replace("\n", " "))
        elif para:
            caption = re.split(r"[\s:]", para, maxsplit=1)[0]
    return out


def fr_n2_protocol_md_examples_match_the_encoder() -> None:
    ex = _examples()
    assert set(ex) == {"HELLO", "COMMAND", "WELCOME", "VERDICT"}
    assert all(len(b) == p.FRAME_LEN == 64 for b in ex.values())
    assert p.encode(p.Hello()) == ex["HELLO"]
    assert p.encode(p.Command(seq=7, duration_ms=1500, sender_ts_ns=1_000_000_000,
                              target_urad=(-523599, 1047198))) == ex["COMMAND"]
    assert p.decode(ex["WELCOME"]) == p.Welcome(state=2, stop_cause=0, has_seq=1, joints=2, last_seq=6,
                                                session=1, telemetry_period_ms=10)
    assert p.decode(ex["VERDICT"]) == p.Verdict(seq=7, result=p.DECIDED, verdict=2, rule=5, state=2, flags=0,
                                                fwd_urad=(-523599, 1047198), fwd_duration_ms=2094,
                                                requested_ms=1500)
    assert p.encode(p.decode(ex["WELCOME"])) == ex["WELCOME"]
    assert p.encode(p.decode(ex["VERDICT"])) == ex["VERDICT"]


ALL_FRAMES: list[p.Frame] = [
    p.Hello(),
    p.Command(seq=0xFFFFFFFF, duration_ms=1, sender_ts_ns=2**64 - 1, target_urad=(-(2**31), 2**31 - 1)),
    p.Estop(),
    p.Welcome(state=1, stop_cause=4, has_seq=0, joints=2, last_seq=0, session=0, telemetry_period_ms=10),
    p.Verdict(seq=1, result=p.UNAVAILABLE, verdict=0, rule=0, state=3, flags=1, fwd_urad=(0, 0),
              fwd_duration_ms=0, requested_ms=100),
    p.Telemetry(index=2**40, t_ns=-5, pos_urad=(-1, 1), vel_urad_s=(-500_000, 500_000), segment=9,
                arm_state=2, lease_ok=1, dropped=3),
    p.State(state=3, stop_cause=4, has_seq=1, last_seq=77, decisions=78, estops=1, watchdog_lost=0,
            submits_refused=2, resets=0),
    p.Error(code=p.E_SLOW),
]


@pytest.mark.parametrize("frame", ALL_FRAMES, ids=lambda f: type(f).__name__)
def fr_n2_every_frame_is_64_bytes_and_round_trips(frame: p.Frame) -> None:
    b = p.encode(frame)
    assert len(b) == 64
    assert b[:4] == b"ARBP" and b[4] == 1 and b[6:8] == b"\0\0"
    assert p.decode(b) == frame


def _hello() -> bytearray:
    return bytearray(p.encode(p.Hello()))


@pytest.mark.parametrize(("offset", "value", "code"), [
    (0, 0x58, p.E_MAGIC),            # magic
    (4, 2, p.E_VERSION),             # version
    (5, 0x7F, p.E_TYPE),             # unknown type
    (6, 1, p.E_RESERVED),            # reserved header byte
    (63, 1, p.E_RESERVED),           # unused body byte
])
def fr_n2_decoder_rejects_malformed_frames(offset: int, value: int, code: int) -> None:
    b = _hello()
    b[offset] = value
    with pytest.raises(p.FrameError) as err:
        p.decode(bytes(b))
    assert err.value.code == code


def fr_n2_decoder_rejects_unused_bytes_after_every_field() -> None:
    """For each type, a non-zero byte just past its last field is refused."""
    last = {p.Command: 32, p.Welcome: 24, p.Verdict: 36, p.Telemetry: 52, p.State: 28, p.Error: 9}
    for frame in ALL_FRAMES:
        end = last.get(type(frame), 8)
        b = bytearray(p.encode(frame))
        b[end] = 1
        with pytest.raises(p.FrameError) as err:
            p.decode(bytes(b))
        assert err.value.code == p.E_RESERVED, type(frame).__name__


@pytest.mark.parametrize("n", [0, 1, 63, 65, 128])
def fr_n2_decoder_rejects_any_other_length(n: int) -> None:
    with pytest.raises(ValueError):
        p.decode(bytes(n))


def fr_n2_command_fields_out_of_range_are_not_encoded() -> None:
    """The encoder refuses values the frame cannot hold; it never wraps them silently."""
    for bad in (p.Command(seq=-1, duration_ms=1, sender_ts_ns=0, target_urad=(0, 0)),
                p.Command(seq=1, duration_ms=2**32, sender_ts_ns=0, target_urad=(0, 0)),
                p.Command(seq=1, duration_ms=1, sender_ts_ns=0, target_urad=(2**31, 0))):
        with pytest.raises(ValueError):
            p.encode(bad)
