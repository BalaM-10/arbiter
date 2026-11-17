"""F8 Malformed input: frames through the gateway and raw devctls on the target."""

from __future__ import annotations

from ... import protocol as pr
from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script, deg
from ..spec import Spec
from .common import baseline

ID = "F8"
TRANSPORT = "gateway"   # decision D6: about the AI side, so through the planner and gateway
FALLS_SILENT = True     # between the two planner sessions
TITLE = "Malformed input"
PASS_CONDITION = (
    "Between two valid planner sessions, six malformed inputs each take the planner connection in turn "
    "(a frame cut short, a 128-byte HELLO, 64 random bytes, a wrong version, a gateway-only type, a "
    "non-zero padding byte): each gets the ERROR the protocol names (none for the cut-short frame, whose "
    "stream ends) and is closed, and none is decided. Meanwhile 40 garbage devctls on /dev/arbiter are "
    "refused and each counted by the guardian as a bad message. Every valid command is ALLOW, the "
    "guardian decides exactly those, and the trusted processes keep running."
)

EXPECTED_ERROR: dict[str, int | None] = {
    "truncated": None, "oversized": pr.E_MAGIC, "random": pr.E_MAGIC, "bad_version": pr.E_VERSION,
    "wrong_type": pr.E_TYPE, "nonzero_padding": pr.E_RESERVED,
}
GARBAGE = 40


def build(spec: Spec) -> Script:
    s = Script()
    s.walk((deg(-15), deg(20)))
    s.dwell(150)
    s.action("garbage", str(GARBAGE))
    for kind in EXPECTED_ERROR:
        s.action("malformed", kind, gap_ms=150)
    s.walk((0, 0))
    s.dwell(150)
    s.status()
    s.end()
    return s


def f8_malformed_input_refused_and_nothing_stalls(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out = [c for c in out if c.name != "no_unexpected_faults"]
    out.append(ck.no_unexpected_faults(run, frozenset({"guardian bad messages"})))
    out.append(ck.verdicts_as_expected(run, script, {"walk": ("ALLOW", "NONE"), "dwell": ("ALLOW", "NONE")}))
    out.append(ck.malformed_refused(run, EXPECTED_ERROR))
    out.append(ck.garbage_counted(run))
    out.append(ck.only_commands_decided(run, script))
    return out


judge = f8_malformed_input_refused_and_nothing_stalls
