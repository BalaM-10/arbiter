"""Fault-bench scenarios played through the planner and the gateway (decision D6).

The scenarios that are about the AI side send their commands the way a planner
does: from this host, over the protocol (docs/PROTOCOL.md), to arb-gateway on the
target, which passes each one to the guardian. On the target, arb-inject sends
nothing; it only records the arm's own telemetry and the guardian's status, and
the bench judges from those exactly as before.

The time of each command, which places the scenario's windows on the arm's
samples, is the guardian's own decision time from its evidence log: the same
monotonic clock as the arm's samples on the same target. The host's clock is not
used for judging.
"""

from __future__ import annotations

from collections import defaultdict, deque

from .. import planner as pl
from .. import protocol as pr
from .records import Verdict
from .script import Script

LEAD_MS = 4000     # arb-inject starts recording before the gateway; the host starts within this


def script_steps(script: Script) -> list[pl.Step]:
    """The script's commands as planner steps: its own sequence numbers, targets,
    durations, and the time to the next command (to the end, for the last)."""
    cmds = script.commands
    out: list[pl.Step] = []
    for i, c in enumerate(cmds):
        nxt = cmds[i + 1].t_ms if i + 1 < len(cmds) else script.end_ms
        out.append(pl.Step(c.target, c.duration_ms, nxt - c.t_ms, c.tag, None, seq=c.seq))
    return out


def record_only_script(script: Script) -> str:
    """arb-inject's script for a gateway scenario: statuses and the end, no command,
    shifted by LEAD_MS so that recording covers the host's whole run."""
    lines = [f"S {t + LEAD_MS}" for t in script.statuses]
    lines.append(f"S {script.end_ms + LEAD_MS}")
    lines.append(f"E {script.end_ms + LEAD_MS + 300}")
    return "\n".join(lines) + "\n"


def verdicts_from(r: pl.RunResult, evlog: list[str]) -> list[Verdict]:
    """The planner's verdicts as the bench's records, each at the guardian's
    decision time from the evidence log (in order, for a sequence number decided
    more than once). A command the guardian did not decide keeps rc = its result."""
    times: dict[int, deque[int]] = defaultdict(deque)
    for line in evlog:
        f = line.split()
        if f and f[0] == "R" and "seq" in f and "t_ns" in f:
            times[int(f[f.index("seq") + 1])].append(int(f[f.index("t_ns") + 1]))
    out: list[Verdict] = []
    last = 0
    for v in r.verdicts:
        t = times[v.seq].popleft() if v.result == pr.DECIDED and times[v.seq] else last
        last = t
        rc = 0 if v.result == pr.DECIDED else v.result
        out.append(Verdict(t, t, 0, v.seq, rc, v.verdict, v.rule, v.state, v.flags, v.fwd_urad,
                           v.fwd_duration_ms, v.requested_ms))
    return out
