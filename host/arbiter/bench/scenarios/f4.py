"""F4 Joint limit violation."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script, deg
from ..spec import Spec
from .common import baseline

ID = "F4"
TITLE = "Joint limit violation"
PASS_CONDITION = (
    "With the arm 5 degrees from the shoulder limit, a shoulder target 10 degrees past its limit and an "
    "elbow target 5 degrees past its limit are each DENY with the joint-limit rule; telemetry shows the "
    "arm holding where it was after each denial and no joint ever beyond its contract limit."
)

NEAR = (deg(-145), 0)


def build(spec: Spec) -> Script:
    s = Script()
    s.walk(NEAR)
    s.dwell(300)
    s.marks["v1"] = s.t_ms
    s.send((deg(-160), 0), 1000, "violation", moves=False)
    s.dwell(450)
    s.marks["v2"] = s.t_ms
    s.send((deg(-145), deg(140)), 1000, "violation", moves=False)
    s.dwell(450)
    s.marks["end"] = s.t_ms
    s.status()
    s.end()
    return s


def f4_joint_limit_denied_and_never_exceeded(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {
        "walk": ("ALLOW", "NONE"), "dwell": ("ALLOW", "NONE"), "violation": ("DENY", "JOINT_LIMIT")}))
    out.append(ck.stayed_at(run, NEAR, script.marks["v1"], script.marks["end"], "arm_held_after_denials"))
    lim = ck.joint_limits_never_exceeded(run, spec)
    margin = lim.data["min_margin_urad"][0]
    out.append(Check("arm_was_near_the_limit", margin < deg(10),
                     f"closest shoulder approach {margin / 1e3:.1f} mrad (must be under 10 deg: the test "
                     "exercised the limit)"))
    return out


judge = f4_joint_limit_denied_and_never_exceeded
