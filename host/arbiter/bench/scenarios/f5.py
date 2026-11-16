"""F5 Keep-out violation: a path into the operator's hand area."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script, deg
from ..spec import Spec
from .common import baseline

ID = "F5"
TITLE = "Keep-out violation"
PASS_CONDITION = (
    "An allowed walk ends with the forearm under 30 mm from the operator's hand area; from there a target "
    "inside the area, a target outside it whose straight path sweeps through it, and a target whose "
    "elbow and tip are both outside the area but whose forearm lies across it are each DENY with the "
    "keep-out rule; telemetry, checked with the bench's own kinematics, shows no part of either link "
    "ever inside the area, and the arm holding where it was after each denial."
)

BESIDE = (deg(18), 0)          # allowed walk's end: forearm about 19 mm below the area's corner
ACROSS = (deg(25), deg(25))    # midpoint review B1: elbow and tip outside, forearm across the area


def build(spec: Spec) -> Script:
    s = Script()
    s.walk(BESIDE)
    s.dwell(300)
    s.marks["v1"] = s.t_ms
    s.send((deg(40), 0), 2000, "violation", moves=False)        # elbow target inside the area
    s.dwell(450)
    s.send((deg(90), 0), 3000, "violation", moves=False)        # target outside, path through it
    s.dwell(450)
    s.send(ACROSS, 2000, "violation", moves=False)              # elbow and tip clear, forearm across
    s.dwell(450)
    s.marks["end"] = s.t_ms
    s.status()
    s.end()
    return s


def f5_keepout_denied_and_never_entered(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {
        "walk": ("ALLOW", "NONE"), "dwell": ("ALLOW", "NONE"), "violation": ("DENY", "KEEPOUT")}))
    out.append(ck.stayed_at(run, BESIDE, script.marks["v1"], script.marks["end"], "arm_held_after_denials"))
    keep = ck.keepout_never_entered(run, spec)
    near = min(keep.data["min_clearance_mm"].values())
    out.append(Check("arm_was_near_the_zone", near < 30.0,
                     f"closest approach {near:.1f} mm during an allowed walk "
                     "(must be under 30 mm: the bench's "
                     "keep-out check was exercised close to the area, M1-J10)"))
    return out


judge = f5_keepout_denied_and_never_entered
