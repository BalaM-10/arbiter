"""Build the timed command scripts that arb-inject plays (a scripted planner).

The planner walks in small joint-space steps, one command every STEP_MS, so it
keeps talking to the guardian well inside the silence timeout and stays at about
half the speed limit. It is deliberately not given the contract (FR-P3): the
scenarios that need a violation write it out explicitly.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

STEP_MS = 150
STEP_URAD = 75_000          # 0.075 rad per step: 0.5 rad/s


@dataclass
class Command:
    t_ms: int
    seq: int
    target: tuple[int, int]
    duration_ms: int
    tag: str                # what the scenario expects of it, e.g. "walk", "violation"


@dataclass
class Script:
    commands: list[Command] = field(default_factory=list)
    statuses: list[int] = field(default_factory=list)
    actions: list[tuple[int, str, str]] = field(default_factory=list)   # (t_ms, name, arg) for arb-inject
    end_ms: int = 0
    t_ms: int = 0
    seq: int = 0
    pos: tuple[int, int] = (0, 0)       # where the planner believes the arm is
    marks: dict[str, int] = field(default_factory=dict)   # named times (ms) or command indexes

    def send(self, target: tuple[int, int], duration_ms: int, tag: str, *, seq: int | None = None,
             gap_ms: int = STEP_MS, moves: bool = True) -> Command:
        """Send one command now, then advance the clock by gap_ms."""
        if seq is None:
            self.seq += 1
            seq = self.seq
        c = Command(self.t_ms, seq, target, duration_ms, tag)
        self.commands.append(c)
        if moves:
            self.pos = target
        self.t_ms += gap_ms
        return c

    def walk(self, target: tuple[int, int], tag: str = "walk") -> int:
        """Step from the current position to target; returns the last command's index."""
        d = max(abs(target[0] - self.pos[0]), abs(target[1] - self.pos[1]))
        n = max(1, math.ceil(d / STEP_URAD))
        start = self.pos
        last: Command | None = None
        for k in range(1, n + 1):
            p = (round(start[0] + (target[0] - start[0]) * k / n),
                 round(start[1] + (target[1] - start[1]) * k / n))
            last = self.send(p, STEP_MS, tag)
        assert last is not None
        return len(self.commands) - 1

    def dwell(self, ms: int, tag: str = "dwell") -> None:
        """Stay at the current position for ms, still talking to the guardian."""
        for _ in range(max(1, ms // STEP_MS)):
            self.send(self.pos, STEP_MS, tag)

    def status(self) -> None:
        self.statuses.append(self.t_ms)

    def action(self, name: str, arg: str = "-", gap_ms: int = 0) -> None:
        """An arb-inject action now (estop; kill @GUARD@ and the like), then advance the clock.
        Only for scenarios that run through arb-inject: its clock is the scenario's."""
        self.actions.append((self.t_ms, name, arg))
        self.t_ms += gap_ms

    def wait(self, ms: int) -> None:
        """Send nothing for ms."""
        self.t_ms += ms

    def end(self, after_ms: int = 300) -> None:
        self.end_ms = self.t_ms + after_ms

    def text(self) -> str:
        steps: list[tuple[int, int, str]] = []
        for i, c in enumerate(self.commands):
            steps.append((c.t_ms, i, f"C {c.t_ms} {c.seq} {c.target[0]} {c.target[1]} {c.duration_ms}"))
        for t in self.statuses:
            steps.append((t, len(self.commands), f"S {t}"))
        for t, name, arg in self.actions:
            steps.append((t, len(self.commands), f"A {t} {name} {arg}"))
        steps.sort()
        lines = [s for _, _, s in steps]
        lines.append(f"E {self.end_ms}")
        return "\n".join(lines) + "\n"


def deg(d: float) -> int:
    return round(d * math.pi / 180.0 * 1e6)
