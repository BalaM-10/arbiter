"""F2 AI hang: the planner falls silent mid-move, then comes back."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script
from ..spec import Spec
from .common import baseline

ID = "F2"
TRANSPORT = "gateway"   # decision D6: about the AI side, so through the planner and gateway
FALLS_SILENT = True     # on purpose: the gap is longer than the silence timeout
TITLE = "AI hang"
PASS_CONDITION = (
    "The planner starts a 2 s move to A, then sends nothing for 800 ms, longer than the contract's silence "
    "timeout: the arm stops short of A and stands still while the planner is silent, and the guardian's "
    "own evidence log shows it in HOLD when the planner's next command arrives. That command and every "
    "later one are ALLOW, the guardian returns to NORMAL, the arm reaches A, and the guardian never enters "
    "SAFE_STOP."
)

A = (-400_000, 200_000)
SILENCE_MS = 800
RESUME_MS = 1500


def build(spec: Spec) -> Script:
    s = Script()
    s.send(A, 2000, "move", gap_ms=SILENCE_MS)              # then silence
    s.marks["resume"] = len(s.commands)
    for k in range(RESUME_MS // 150):                      # back, and talking again; the time left shrinks
        s.send(A, RESUME_MS - 150 * k, "resume")
    for _ in range(3):
        s.send(A, 150, "resume")
    s.status()
    s.end()
    return s


def f2_planner_silence_holds_the_arm_then_resumes(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {"move": ("ALLOW", "NONE"), "resume": ("ALLOW", "NONE")}))
    # Stopped well after the silence timeout, until just before the planner returns.
    out.append(ck.stopped_short(run, A, spec.silence_timeout_ms + 200, SILENCE_MS - 20,
                                "arm_stopped_short_while_the_planner_was_silent"))
    resume = script.commands[script.marks["resume"]]
    out.append(ck.hold_recorded_before(run, resume.seq))
    out.append(ck.reached(run, resume, "reached_A_after_resuming"))
    out.append(ck.never_safe_stop(run))
    return out


judge = f2_planner_silence_holds_the_arm_then_resumes
