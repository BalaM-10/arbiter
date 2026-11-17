"""F6 Over-speed: commands demanding motion faster than the limit."""

from __future__ import annotations

import math

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script
from ..spec import Spec
from .common import baseline

ID = "F6"
TRANSPORT = "gateway"   # decision D6: about the AI side, so through the planner and gateway
TITLE = "Over-speed"
PASS_CONDITION = (
    "Commands asking for up to 10 times the speed limit are CLAMP with the velocity rule, and the first "
    "one's forwarded duration is at least distance / limit; the joint speed computed from telemetry "
    "positions and times never exceeds the contract limit, while reaching at least 95% of it (the test "
    "drove the arm at the limit); both targets are reached."
)

TARGET = (-400_000, 400_000)      # 0.4 rad per joint
HOME = (0, 0)


def build(spec: Spec) -> Script:
    s = Script()
    for name, tgt, ask_ms in (("target", TARGET, 40), ("home", HOME, 40)):
        s.marks[f"first_{name}"] = len(s.commands)
        s.send(tgt, ask_ms, "overspeed")        # 0.4 rad in 40 ms = 10 x the limit
        s.send(tgt, 20, "overspeed")            # re-asked mid-move, still too fast
        s.send(tgt, 20, "overspeed")
        s.marks[f"arrive_{name}"] = len(s.commands) - 1
        s.send(tgt, 150, "hold")
        s.send(tgt, 150, "hold")
    s.status()
    s.end()
    return s


def f6_overspeed_clamped_and_never_above_limit(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {"overspeed": ("CLAMP", "VELOCITY"),
                                                     "hold": ("ALLOW", "NONE")}))
    need = math.ceil(max(abs(TARGET[0]), abs(TARGET[1])) / spec.vmax_urad_s * 1000.0)
    for name in ("target", "home"):
        i = script.marks[f"first_{name}"]
        if i < len(run.verdicts):
            v = run.verdicts[i]
            out.append(Check(f"clamped_duration_seq{v.seq}", v.duration_ms >= need,
                             f"asked {v.requested_ms} ms, forwarded {v.duration_ms} ms, needs >= {need} ms"))
    _, peak = ck.speed_never_above_limit(run, spec)
    out.append(Check("arm_moved_at_the_limit", peak >= 0.95 * spec.vmax_urad_s,
                     f"peak {peak / 1e6:.4f} rad/s raw, at least 95% of the limit "
                     f"{spec.vmax_urad_s / 1e6:.4f} rad/s (the test drove the arm at the limit)"))
    for name in ("target", "home"):
        out.append(ck.reached(run, script.commands[script.marks[f"arrive_{name}"]], f"reached_{name}"))
    return out


judge = f6_overspeed_clamped_and_never_above_limit
