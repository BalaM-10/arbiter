"""Host tests of the scripted planner (FR-P1, FR-P3, FR-P4), `arbiter.planner`.

The planner talks to a fake gateway here: a small TCP server in this file that
speaks docs/PROTOCOL.md through `arbiter.protocol` and decides nothing (it
allows every command and reports the arm at the last target). The planner on
QNX, through the real gateway, guardian and arm, is
host/arbiter/planner_e2e.py::fr_p1_scripted_planner_runs_pick_and_place_through_the_gateway.
"""

from __future__ import annotations

import ast
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from arbiter import planner as pl
from arbiter import protocol as p

PLANNER_SRC = Path(pl.__file__)


class FakeGateway:
    """One planner connection; ALLOW for every command; TELEMETRY at the last target."""

    def __init__(self, *, has_seq: int = 0, last_seq: int = 0, error_after: int | None = None) -> None:
        self.has_seq, self.last_seq, self.error_after = has_seq, last_seq, error_after
        self.commands: list[p.Command] = []
        self.bad_frames = 0
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self) -> None:
        conn, _ = self.sock.accept()
        conn.settimeout(10)
        pos = (0, 0)
        index = 0
        with conn:
            while True:
                try:
                    b = conn.recv(p.FRAME_LEN, socket.MSG_WAITALL)
                except OSError:
                    return
                if len(b) < p.FRAME_LEN:
                    return
                try:
                    f = p.decode(b)
                except ValueError:
                    self.bad_frames += 1
                    return
                out: list[p.Frame] = []
                if isinstance(f, p.Hello):
                    out = [p.Welcome(2, 0, self.has_seq, 2, self.last_seq, 1, 10),
                           p.State(2, 0, self.has_seq, self.last_seq, 0, 0, 0, 0, 0)]
                elif isinstance(f, p.Command):
                    self.commands.append(f)
                    if self.error_after is not None and len(self.commands) > self.error_after:
                        conn.sendall(p.encode(p.Error(p.E_UNAVAILABLE)))
                        return
                    pos = f.target_urad
                    index += 10
                    out = [p.Verdict(f.seq, p.DECIDED, 1, 0, 2, 0, f.target_urad, f.duration_ms,
                                     f.duration_ms),
                           p.Telemetry(index, index * 1_000_000, pos, (0, 0), len(self.commands), 1, 1, 0)]
                for o in out:
                    conn.sendall(p.encode(o))


def _quick_plan() -> list[pl.Step]:
    """The real path with no waiting between commands (the fake arm arrives at once)."""
    return [pl.Step(s.target_urad, s.duration_ms, 0, s.tag, s.waypoint) for s in pl.scripted_plan()]


def fr_p1_scripted_plan_is_the_fixed_pick_and_place_path() -> None:
    a, b = pl.scripted_plan(), pl.scripted_plan()
    assert a == b                                            # fixed: the same every time
    assert [s.waypoint for s in a if s.waypoint] == ["approach", "pick", "lift", "place", "home"]
    assert a[-1].target_urad == (0, 0)                       # ends at home
    for name, (d0, d1) in pl.PICK_AND_PLACE:
        last = [s for s in a if s.waypoint == name][-1]
        assert last.target_urad == (pl.deg(d0), pl.deg(d1))
    assert all(s.duration_ms > 0 and s.gap_ms > 0 for s in a)


def fr_p1_planner_resumes_after_the_guardians_last_sequence_number() -> None:
    gw = FakeGateway(has_seq=1, last_seq=41)
    plan = _quick_plan()
    r = pl.run("127.0.0.1", gw.port, plan)
    assert r.error is None and r.welcome is not None and r.welcome.last_seq == 41
    assert [c.seq for c in gw.commands] == list(range(42, 42 + len(plan)))
    assert [c.target_urad for c in gw.commands] == [s.target_urad for s in plan]
    assert [v.seq for v in r.verdicts] == [c.seq for c in gw.commands]
    assert gw.bad_frames == 0
    assert pl.check(r, plan) == []


def fr_p1_planner_starts_at_one_when_the_guardian_has_no_sequence_yet() -> None:
    gw = FakeGateway(has_seq=0, last_seq=0)
    r = pl.run("127.0.0.1", gw.port, _quick_plan()[:3])
    assert r.error is None
    assert [c.seq for c in gw.commands] == [1, 2, 3]


def fr_p1_planner_sequence_wraps_at_2_32() -> None:
    gw = FakeGateway(has_seq=1, last_seq=0xFFFFFFFE)
    r = pl.run("127.0.0.1", gw.port, _quick_plan()[:3])
    assert r.error is None
    assert [c.seq for c in gw.commands] == [0xFFFFFFFF, 0, 1]


def fr_p1_planner_stops_at_an_error_frame_and_says_so() -> None:
    gw = FakeGateway(error_after=2)
    plan = _quick_plan()
    r = pl.run("127.0.0.1", gw.port, plan)
    assert r.error == p.E_UNAVAILABLE
    assert len(gw.commands) == 3 and len(r.verdicts) == 2
    assert any("ERROR 7" in problem for problem in pl.check(r, plan))


def fr_p1_check_reports_denials_and_unreached_waypoints() -> None:
    plan = _quick_plan()[:2]
    v_ok = p.Verdict(1, p.DECIDED, 1, 0, 2, 0, plan[0].target_urad, 1, 1)
    v_deny = p.Verdict(2, p.DECIDED, 3, 6, 2, 0, (0, 0), 0, 1)
    r = pl.RunResult(welcome=None, verdicts=[v_ok, v_deny], commands=[], telemetry=[], states=[], error=None)
    problems = pl.check(r, plan)
    assert any("DENY" in x and "KEEPOUT" in x for x in problems)


def fr_p3_planner_never_reads_the_contract() -> None:
    """The planner has no contract, imports nothing that reads one, and names no contract file."""
    tree = ast.parse(PLANNER_SRC.read_text(encoding="utf-8"))
    imported: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported |= {a.name for a in node.names}
        elif isinstance(node, ast.ImportFrom):
            imported.add(("." * node.level) + (node.module or ""))
            imported |= {a.name for a in node.names}
    forbidden = {"contract", ".contract", "arbiter.contract", "table", ".table", "arbiter.table", "yaml",
                 "spec", "bench"}
    assert not imported & forbidden, imported
    assert "contracts/" not in PLANNER_SRC.read_text(encoding="utf-8")


def fr_p3_planner_sends_a_wrong_target_unchanged() -> None:
    """It must be able to be wrong: nothing in the planner clamps or filters a target."""
    gw = FakeGateway()
    wild = [pl.Step((pl.deg(-400), pl.deg(400)), 1, 0, "wrong", None)]
    r = pl.run("127.0.0.1", gw.port, wild)
    assert r.error is None
    assert gw.commands[0].target_urad == (pl.deg(-400), pl.deg(400))


def fr_p4_scripted_planner_runs_with_no_key_in_the_environment() -> None:
    """`arbiter planner run` with an environment holding nothing but PATH."""
    gw = FakeGateway()
    env = {"PATH": os.environ.get("PATH", "")}
    t0 = time.monotonic()
    r = subprocess.run([sys.executable, "-I", "-m", "arbiter.cli", "planner", "run", "--host", "127.0.0.1",
                        "--port", str(gw.port), "--no-wait"],
                       env=env, cwd=Path(pl.__file__).resolve().parents[1], capture_output=True, text=True,
                       timeout=60, check=False)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "SUMMARY planner" in r.stdout
    assert len(gw.commands) == len(pl.scripted_plan())
    assert time.monotonic() - t0 < 60


@pytest.mark.parametrize("name", ["KEY", "TOKEN", "SECRET", "PASSWORD"])
def fr_p4_planner_reads_no_credential_from_anywhere(name: str) -> None:
    """The scripted planner reads no credential at all: none from the environment, none from a file."""
    src = PLANNER_SRC.read_text(encoding="utf-8")
    assert name not in src.upper().replace("API KEY", "")
