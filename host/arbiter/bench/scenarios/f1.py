"""F1 Normal operation: the scripted pick-and-place path."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script, deg
from ..spec import Spec
from .common import baseline

ID = "F1"
TRANSPORT = "gateway"   # decision D6: about the AI side, so through the planner and gateway
TITLE = "Normal operation"
PASS_CONDITION = (
    "Every command of the pick-and-place path is ALLOW; telemetry shows the arm reaching every waypoint "
    "while it pauses there; no deadline miss: no lease expiry, no guardian-to-arm send fault, the lease "
    "valid whenever the arm moves, and no telemetry interval of 2 control periods or more (strict on the "
    "Pi 5; on QEMU measured and reported, not judged, because QEMU proves function, never timing); and "
    "the independent checks find no limit, keep-out or speed violation."
)

# Waypoints in degrees (shoulder, elbow): all on the far side of the bench from
# the operator's hand area.
WAYPOINTS = [("approach", (-30, 60)), ("pick", (-45, 90)), ("lift", (-30, 60)),
             ("place", (-100, 45)), ("home", (0, 0))]
DWELL_MS = 450              # at pick and place
PAUSE_MS = 300              # at every other waypoint, so arrival is observable


def build(spec: Spec) -> Script:
    s = Script()
    for name, (a, b) in WAYPOINTS:
        s.marks[name] = s.walk((deg(a), deg(b)))
        s.dwell(DWELL_MS if name in ("pick", "place") else PAUSE_MS)
        s.marks[f"{name}_until"] = s.t_ms
    s.status()
    s.end()
    return s


def f1_pick_and_place_all_allowed_reached_no_deadline_miss(
        run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {"walk": ("ALLOW", "NONE"), "dwell": ("ALLOW", "NONE")}))
    out.append(ck.never_safe_stop(run))
    out += [ck.reached(run, script.commands[script.marks[name]], f"reached_{name}",
                       script.marks[f"{name}_until"]) for name, _ in WAYPOINTS]
    out.append(ck.no_missed_cycle(run, script))
    return out


judge = f1_pick_and_place_all_allowed_reached_no_deadline_miss
