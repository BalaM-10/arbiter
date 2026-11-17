"""F7 Repeated violations: SAFE_STOP, stopped until the operator resets."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script, deg
from ..spec import Spec
from .common import baseline

ID = "F7"
TRANSPORT = "gateway"   # decision D6: about the AI side, so through the planner and gateway
FALLS_SILENT = True     # while the operator resets, the planner waits
TITLE = "Repeated violations"
PASS_CONDITION = (
    "While the arm moves to A, the planner sends three joint-limit violations inside one second (the "
    "contract's threshold and window): each is DENY with the joint-limit rule and the guardian enters "
    "SAFE_STOP; the arm brakes and stands still, and valid commands are DENY with the state rule. After "
    "the operator's reset at the target (arb-ctl as the operator's user), the guardian's own record shows "
    "HOLD, the next commands are ALLOW and the arm reaches B."
)

A = (-400_000, 200_000)
B = (-200_000, 100_000)
BAD = (deg(-170), 0)                         # past the shoulder's limit
RESET_MS = 1000
RESUME_MS = 1800


def build(spec: Spec) -> Script:
    s = Script()
    s.send(A, 2000, "move")
    for _ in range(3):
        s.send(BAD, 1000, "violation", moves=False)
    s.marks["stopped"] = s.t_ms
    for _ in range(2):
        s.send(A, 2000, "after", moves=False)
    s.t_ms = RESET_MS
    s.action("reset")
    s.t_ms = RESUME_MS
    s.marks["resumed"] = len(s.commands)
    for k in range(10):                      # the time left shrinks, as in F2
        s.send(B, 1500 - 150 * k, "resumed")
    s.send(B, 150, "resumed")
    s.status()
    s.end()
    return s


def f7_violations_safe_stop_until_operator_reset(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {"move": ("ALLOW", "NONE"),
                                                     "violation": ("DENY", "JOINT_LIMIT"),
                                                     "after": ("DENY", "STATE"),
                                                     "resumed": ("ALLOW", "NONE")}))
    out.append(ck.stopped_short(run, A, script.marks["stopped"] + 150, RESET_MS - 50,
                                "arm_braked_and_still_in_safe_stop"))
    out.append(ck.host_action_done(run, "reset"))
    first = script.commands[script.marks["resumed"]]
    out.append(ck.hold_recorded_before(run, first.seq))
    out.append(ck.reached(run, first, "reached_B_after_the_reset"))
    return out


judge = f7_violations_safe_stop_until_operator_reset
