"""F14 Emergency stop mid-move."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script
from ..spec import Spec
from .common import baseline

ID = "F14"
TRANSPORT = "inject"    # decision D6: on-target timing, so a tool on the target
TITLE = "Emergency stop"
PASS_CONDITION = (
    "While the arm moves to A, the emergency stop is pressed: the arm brakes within 2 control cycles "
    "(judged on the Pi 5, measured and reported on QEMU) and never moves again, the guardian is in "
    "SAFE_STOP with the e-stop as its cause, and every later command is DENY with the state rule."
)

A = (-400_000, 200_000)


def build(spec: Spec) -> Script:
    s = Script()
    s.send(A, 2000, "move", gap_ms=150)
    s.status()
    s.action("estop")
    s.wait(80)
    s.send(A, 2000, "after")
    s.send((0, 0), 2000, "after")
    s.status()
    s.end()
    return s


def f14_estop_brakes_and_stays_stopped(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {"move": ("ALLOW", "NONE"), "after": ("DENY", "STATE")}))
    out.append(ck.braked_within(run, "estop", 2, "arm_braked_after_the_estop"))
    out.append(ck.safe_stop_by(run, "ESTOP"))
    return out


judge = f14_estop_brakes_and_stays_stopped
