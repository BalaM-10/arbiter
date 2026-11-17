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

LEAD_MS = 6000     # arb-inject starts recording before the gateway; the host starts within this,
                   # and may run this much late (host actions over SSH take time)


# Actions done from this host, at their time in the plan (an operator at the target's
# console, someone killing the gateway, a client sending malformed frames), and
# actions arb-inject does on the target. Only "garbage" may run on the target in a
# gateway scenario: its clock is only roughly aligned with the planner's.
HOST_ACTIONS = frozenset({"reset", "kill_gateway", "malformed"})
TARGET_ACTIONS_VIA_GATEWAY = frozenset({"garbage"})


def script_steps(script: Script) -> list[pl.Step]:
    """The script's commands and host actions as planner steps, in time order: each
    command with its own sequence number, target and duration; each entry's gap is the
    time to the next entry (to the end, for the last)."""
    entries: list[tuple[int, int, pl.Step]] = []
    for i, c in enumerate(script.commands):
        entries.append((c.t_ms, 2 * i + 1, pl.Step(c.target, c.duration_ms, 0, c.tag, None, seq=c.seq)))
    for t, name, arg in script.actions:
        if name in HOST_ACTIONS:
            # Before a command planned for the same time, after the ones planned earlier.
            before = sum(1 for c in script.commands if c.t_ms < t)
            entries.append((t, 2 * before, pl.Step((0, 0), 0, 0, name, None, action=name, arg=arg)))
        elif name not in TARGET_ACTIONS_VIA_GATEWAY:
            raise ValueError(f"action {name} cannot run in a scenario played through the gateway")
    entries.sort(key=lambda e: (e[0], e[1]))
    out: list[pl.Step] = []
    for k, (t, _, st) in enumerate(entries):
        nxt = entries[k + 1][0] if k + 1 < len(entries) else script.end_ms
        out.append(pl.Step(st.target_urad, st.duration_ms, nxt - t, st.tag, None, seq=st.seq,
                           action=st.action, arg=st.arg))
    return out


def sessions(plan: list[pl.Step]) -> list[list[pl.Step]]:
    """The plan cut into planner sessions: a "malformed" entry needs the planner slot,
    so the session before it ends there, and the next one starts after it."""
    out: list[list[pl.Step]] = []
    cur: list[pl.Step] = []
    for st in plan:
        if st.action == "malformed":
            if cur:
                out.append(cur)
                cur = []
            out.append([st])
        else:
            cur.append(st)
    if cur:
        out.append(cur)
    return out


def record_only_script(script: Script) -> str:
    """arb-inject's script for a gateway scenario: statuses, target actions and the end,
    no command, shifted by LEAD_MS so that recording covers the host's whole run."""
    steps = [(t + LEAD_MS, f"S {t + LEAD_MS}") for t in script.statuses]
    steps += [(t + LEAD_MS, f"A {t + LEAD_MS} {name} {arg}") for t, name, arg in script.actions
              if name in TARGET_ACTIONS_VIA_GATEWAY]
    steps.sort()
    lines = [x for _, x in steps]
    lines.append(f"S {script.end_ms + LEAD_MS}")
    lines.append(f"E {script.end_ms + LEAD_MS + 300}")
    return "\n".join(lines) + "\n"


# Malformed first frames, sent on the planner port when no planner is connected (F8).
def _frame(kind: str) -> tuple[bytes, bool]:
    """The bytes for one kind of malformed input, and whether a HELLO goes first."""
    import random

    rnd = random.Random(8)                                    # the same bytes every run
    cmd = bytearray(pr.encode(pr.Command(1, 100, 0, (0, 0))))
    if kind == "truncated":
        return bytes(cmd[:40]), False                         # then end of stream
    if kind == "oversized":
        # A 128-byte HELLO: frames are 64 bytes, so the 64 past the boundary are read as
        # the next frame, which is not one. (Fewer would wait as a partial frame; a
        # command in front would be decided, correctly: on a stream of fixed frames its
        # first 64 bytes are a frame.)
        junk = bytes(rnd.randrange(1, 256) for _ in range(63))
        return pr.encode(pr.Hello()) + b"Z" + junk, False
    if kind == "random":
        b = bytearray(rnd.randrange(256) for _ in range(64))
        b[0] = 0x5A if b[0] == 0x41 else b[0]                 # never the magic by chance
        return bytes(b), False
    if kind == "bad_version":
        cmd[4] = 2
        return bytes(cmd), True
    if kind == "wrong_type":
        cmd[5] = 0x82                                         # a gateway-to-client type
        return bytes(cmd), True
    if kind == "nonzero_padding":
        cmd[63] = 1
        return bytes(cmd), True
    raise ValueError(kind)


def malformed(host: str, kind: str) -> dict[str, object]:
    """Send one kind of malformed input on a fresh planner connection; report the ERROR
    code the gateway sent (None if none), whether it closed, and whether anything was
    decided (a VERDICT must never come back)."""
    import socket

    data, hello = _frame(kind)
    ev: dict[str, object] = {"name": "malformed", "arg": kind, "error": None, "closed": False, "verdicts": 0}
    try:
        with socket.create_connection((host, pr.PLANNER_PORT), timeout=3) as s:
            s.settimeout(3)
            if hello:
                s.sendall(pr.encode(pr.Hello()))
            s.sendall(data)
            if kind == "truncated":
                s.shutdown(socket.SHUT_WR)
            buf = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    ev["closed"] = True
                    break
                buf += chunk
                while len(buf) >= pr.FRAME_LEN:
                    f = pr.decode(buf[:pr.FRAME_LEN])
                    buf = buf[pr.FRAME_LEN:]
                    if isinstance(f, pr.Error):
                        ev["error"] = f.code
                    elif isinstance(f, pr.Verdict):
                        ev["verdicts"] = int(str(ev["verdicts"])) + 1
    except (OSError, ValueError) as err:
        ev["exception"] = str(err)
    return ev


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
