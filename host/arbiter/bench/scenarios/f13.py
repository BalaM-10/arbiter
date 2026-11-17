"""F13 Guardian killed mid-move."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script
from ..spec import Spec

ID = "F13"
TRANSPORT = "inject"    # decision D6: a trusted process killed, so a tool on the target
TITLE = "Guardian killed"
PASS_CONDITION = (
    "While the arm moves to A, the guardian is killed (SIGKILL): the arm brakes on its own within 5 "
    "control cycles (NFR-T5; judged on the Pi 5, measured and reported on QEMU) and never moves again, "
    "the watchdog records that the guardian died or disconnected, the watchdog and the arm keep running, "
    "and no limit, keep-out or speed rule is broken."
)

A = (-400_000, 200_000)


def build(spec: Spec) -> Script:
    s = Script()
    s.send(A, 2000, "move", gap_ms=150)
    s.status()
    s.wait(50)
    s.action("kill", "@GUARD@")
    s.wait(1200)
    s.end(after_ms=0)
    return s


def _alive_but_the_guardian(run: Run) -> Check:
    ok = run.alive == {"watchdog": 1, "arm": 1, "guardian": 0}
    return Check("watchdog_and_arm_alive_guardian_gone", ok, f"after the run: {run.alive}")


def f13_guardian_killed_arm_brakes_watchdog_records(run: Run, script: Script, spec: Spec) -> list[Check]:
    speed, _ = ck.speed_never_above_limit(run, spec)
    return [
        ck.run_complete(run, script),
        ck.contract_matches(run, spec),
        ck.telemetry_complete(run),
        _alive_but_the_guardian(run),
        # The kill's own consequences: the watchdog's trip, and the beats it can no longer
        # deliver to the dead guardian; the arm's disconnect, watchdog brake and lease expiry.
        ck.no_unexpected_faults(run, frozenset({"watchdog trips", "watchdog beat errors", "arm disconnects",
                                                "arm watchdog brakes", "arm lease expiries"})),
        ck.joint_limits_never_exceeded(run, spec),
        ck.keepout_never_entered(run, spec),
        speed,
        ck.verdicts_as_expected(run, script, {"move": ("ALLOW", "NONE")}),
        ck.braked_within(run, "kill", 5, "arm_braked_after_the_guardian_died"),
        ck.watchdog_recorded(run, "guardian", ("died", "disconnected")),
    ]


judge = f13_guardian_killed_arm_brakes_watchdog_records
