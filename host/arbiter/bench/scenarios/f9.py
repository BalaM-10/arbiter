"""F9 Replay: old sequence numbers resent with a different target."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script
from ..spec import Spec
from .common import baseline

ID = "F9"
TRANSPORT = "gateway"   # decision D6: about the AI side, so through the planner and gateway
TITLE = "Replay"
PASS_CONDITION = (
    "While the arm moves to target A, commands reusing the current sequence number or an older one, "
    "aimed at target B, are DENY with the sequence rule, and so is an exact duplicate of an accepted "
    "command; fresh sequence numbers are still ALLOW; telemetry shows the arm only ever on the straight "
    "joint-space line from home to A, never heading for B."
)

A = (-200_000, 100_000)
B = (-600_000, -300_000)


def build(spec: Spec) -> Script:
    s = Script()
    s.send(A, 300, "fresh", seq=100, gap_ms=150)
    s.send(B, 300, "replay", seq=100, gap_ms=50, moves=False)     # same seq as the accepted one
    s.send(B, 300, "replay", seq=60, gap_ms=100, moves=False)     # older seq
    s.send(A, 150, "fresh", seq=101, gap_ms=100)
    s.send(A, 150, "replay", seq=101, gap_ms=50, moves=False)     # exact duplicate
    s.send(A, 150, "fresh", seq=102)
    s.send(A, 150, "fresh", seq=103)
    s.status()
    s.end()
    return s


def f9_replay_denied_and_arm_never_heads_for_it(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {"fresh": ("ALLOW", "NONE"),
                                                     "replay": ("DENY", "SEQUENCE")}))
    out.append(ck.on_segment(run, (0, 0), A, "arm_only_on_home_to_A"))
    out.append(ck.reached(run, script.commands[0], "reached_A"))
    return out


judge = f9_replay_denied_and_arm_never_heads_for_it
