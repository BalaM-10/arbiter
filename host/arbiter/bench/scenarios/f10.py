"""F10 AI process killed: the gateway killed, and the planner with it, mid-move."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script
from ..spec import Spec
from .common import baseline

ID = "F10"
TRANSPORT = "gateway"   # decision D6: about the AI side, so through the planner and gateway
TITLE = "AI process killed"
PASS_CONDITION = (
    "While the arm moves to A, the gateway is killed on the target (SIGKILL) and the planner stops with "
    "it: the guardian goes to HOLD, the arm stops short of A and stands still, and the watchdog, the arm "
    "and the guardian run on with no trip, fault or brake."
)

A = (-400_000, 200_000)
KILL_MS = 450
AFTER_MS = 1500


def build(spec: Spec) -> Script:
    s = Script()
    for k in range(3):                       # moving, and talking
        s.send(A, 2000 - 150 * k, "move")
    s.t_ms = KILL_MS
    s.action("kill_gateway")
    s.t_ms = KILL_MS + AFTER_MS
    s.status()
    s.end(after_ms=0)
    return s


def f10_planner_and_gateway_killed_arm_holds(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {"move": ("ALLOW", "NONE")}))
    out.append(ck.host_action_done(run, "kill_gateway"))
    out.append(ck.stopped_short(run, A, KILL_MS + 400, KILL_MS + AFTER_MS - 200,
                                "arm_stopped_short_after_the_kill"))
    out.append(ck.final_state(run, "HOLD"))
    return out


judge = f10_planner_and_gateway_killed_arm_holds
