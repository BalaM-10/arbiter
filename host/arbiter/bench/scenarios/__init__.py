"""One module per fault scenario (fault-bench skill: ID, TITLE, PASS_CONDITION,
build() -> Script, judge() -> list of Checks). M1 set: F1, F4, F5, F6, F9."""

from __future__ import annotations

from types import ModuleType

from . import f1, f4, f5, f6, f9

ALL: dict[str, ModuleType] = {m.ID: m for m in (f1, f4, f5, f6, f9)}
