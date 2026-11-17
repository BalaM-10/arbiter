"""F15 Evidence tampering: one byte of the scenario's own evidence log changed."""

from __future__ import annotations

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script, deg
from ..spec import Spec
from .common import baseline

ID = "F15"
TRANSPORT = "gateway"   # a short planner session, to make a real evidence log
TITLE = "Evidence tampering"
PASS_CONDITION = (
    "A short planner session is decided and logged by the target's evidence logger; its log verifies as "
    "written. Then one byte of one decision record is changed: `arbiter evidence verify` fails, and the "
    "first error it reports names that record's line."
)


def build(spec: Spec) -> Script:
    s = Script()
    s.walk((deg(-20), deg(30)))
    s.dwell(300)
    s.status()
    s.end()
    return s


def f15_one_changed_byte_is_found_and_named(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)
    out.append(ck.verdicts_as_expected(run, script, {"walk": ("ALLOW", "NONE"), "dwell": ("ALLOW", "NONE")}))
    out.append(ck.tampering_detected(run))
    return out


judge = f15_one_changed_byte_is_found_and_named
