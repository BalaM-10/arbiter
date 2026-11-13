"""Checks every scenario runs, whatever it injects."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script
from ..spec import Spec


def baseline(run: Run, script: Script, spec: Spec) -> list[Check]:
    speed, _ = ck.speed_never_above_limit(run, spec)
    return [
        ck.run_complete(run, script),
        ck.contract_matches(run, spec),
        ck.telemetry_complete(run),
        ck.trusted_alive(run),
        ck.joint_limits_never_exceeded(run, spec),
        ck.keepout_never_entered(run, spec),
        speed,
    ]
