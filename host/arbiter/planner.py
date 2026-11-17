"""The scripted planner (FR-P1): plays a fixed pick-and-place path to the gateway.

It needs no network beyond the link to the gateway, and no credential of any
kind. It speaks only the protocol (docs/PROTOCOL.md, `arbiter.protocol`), so the
gateway cannot tell it from any other planner; an LLM planner (FR-P2) will send
the same frames.

It is not given the contract (FR-P3): it knows nothing of joint limits, the speed
limit or the keep-out zones, and sends whatever its path says. Keeping the arm
safe is the guardian's job, never the planner's. The path below stays clear of
every limit only because it was written that way, the same path as fault
scenario F1.

    arbiter planner run --host <target> [--port 7700] [--no-wait]
"""

from __future__ import annotations

import math
import select
import socket
import time
from collections.abc import Callable
from dataclasses import dataclass

from . import protocol as p

# Waypoints in degrees (shoulder, elbow), on the far side of the bench from the
# operator's hand area: the path of fault scenario F1.
PICK_AND_PLACE: list[tuple[str, tuple[float, float]]] = [
    ("approach", (-30, 60)), ("pick", (-45, 90)), ("lift", (-30, 60)), ("place", (-100, 45)),
    ("home", (0, 0))]
STEP_MS = 150                   # one command every 150 ms: well inside any silence timeout
STEP_URAD = 75_000              # 0.075 rad per step: about 0.5 rad/s
DWELL_MS = {"pick": 450, "place": 450}
PAUSE_MS = 300                  # at every other waypoint
REACHED_TOL_URAD = 10           # as the fault bench (host/arbiter/bench/checks.py)
SOCKET_TIMEOUT_S = 5.0
TAIL_S = 0.5                    # telemetry kept after the last verdict


def deg(d: float) -> int:
    return round(d * math.pi / 180.0 * 1e6)


@dataclass(frozen=True)
class Step:
    target_urad: tuple[int, int]
    duration_ms: int
    gap_ms: int                 # wait before the next command
    tag: str                    # "walk", "dwell", or anything a caller wants
    waypoint: str | None        # set on the command that ends a walk to a named waypoint


def scripted_plan(start: tuple[int, int] = (0, 0)) -> list[Step]:
    """The fixed path: walk to each waypoint in small steps, then pause there."""
    out: list[Step] = []
    pos = start
    for name, (a, b) in PICK_AND_PLACE:
        target = (deg(a), deg(b))
        n = max(1, math.ceil(max(abs(target[0] - pos[0]), abs(target[1] - pos[1])) / STEP_URAD))
        for k in range(1, n + 1):
            q = (round(pos[0] + (target[0] - pos[0]) * k / n), round(pos[1] + (target[1] - pos[1]) * k / n))
            out.append(Step(q, STEP_MS, STEP_MS, "walk", name if k == n else None))
        pos = target
        for _ in range(max(1, DWELL_MS.get(name, PAUSE_MS) // STEP_MS)):
            out.append(Step(pos, STEP_MS, STEP_MS, "dwell", None))
    return out


@dataclass
class RunResult:
    welcome: p.Welcome | None
    verdicts: list[p.Verdict]
    commands: list[p.Command]
    telemetry: list[tuple[int, p.Telemetry]]   # (verdicts received when it arrived, frame)
    states: list[p.State]
    error: int | None                          # ERROR code from the gateway, if any


class _Conn:
    """Frames in and out of one TCP connection; collects what is not asked for."""

    def __init__(self, host: str, port: int, result: RunResult) -> None:
        self.sock = socket.create_connection((host, port), timeout=SOCKET_TIMEOUT_S)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.r = result

    def send(self, f: p.Frame) -> None:
        self.sock.sendall(p.encode(f))

    def _one(self, timeout: float) -> p.Frame | None:
        """The next frame within timeout seconds, or None."""
        end = time.monotonic() + timeout
        while len(self.buf) < p.FRAME_LEN:
            left = end - time.monotonic()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                return None
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("the gateway closed the connection")
            self.buf += chunk
        b, self.buf = self.buf[:p.FRAME_LEN], self.buf[p.FRAME_LEN:]
        f = p.decode(b)
        if isinstance(f, p.Telemetry):
            self.r.telemetry.append((len(self.r.verdicts), f))
        elif isinstance(f, p.State):
            self.r.states.append(f)
        elif isinstance(f, p.Error):
            self.r.error = f.code
        return f

    def until(self, want: type, timeout: float = SOCKET_TIMEOUT_S) -> p.Frame | None:
        """Read until a frame of type want (returned), an ERROR, or the timeout."""
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            f = self._one(left)
            if f is None or isinstance(f, p.Error):
                return None
            got: p.Frame = f
            if type(got).__name__ == want.__name__:
                return got
        return None

    def pump(self, seconds: float) -> None:
        """Keep reading (telemetry, state) for this long."""
        end = time.monotonic() + seconds
        while (left := end - time.monotonic()) > 0 and self.r.error is None:
            if self._one(left) is None:
                return

    def close(self) -> None:
        self.sock.close()


def run(host: str, port: int, plan: list[Step], *, wait: bool = True,
        log: Callable[[str], None] | None = None) -> RunResult:
    """Say HELLO, then send each step as a COMMAND, resuming after the guardian's
    last sequence number, and wait for its VERDICT. Stops at an ERROR frame."""
    r = RunResult(None, [], [], [], [], None)
    c = _Conn(host, port, r)
    try:
        c.send(p.Hello())
        w = c.until(p.Welcome)
        if not isinstance(w, p.Welcome):
            return r
        r.welcome = w
        seq = (w.last_seq + 1) & 0xFFFFFFFF if w.has_seq else 1
        if log:
            log(f"WELCOME state {p.STATES.get(w.state, w.state)} has_seq {w.has_seq} last_seq {w.last_seq} "
                f"session {w.session}: first seq {seq}")
        for step in plan:
            t0 = time.monotonic()
            cmd = p.Command(seq, step.duration_ms, time.time_ns(), step.target_urad)
            c.send(cmd)
            r.commands.append(cmd)
            v = c.until(p.Verdict)
            if not isinstance(v, p.Verdict):
                break
            r.verdicts.append(v)
            if log:
                log(f"C seq {seq} target {step.target_urad[0]} {step.target_urad[1]} dur {step.duration_ms} "
                    f"{step.tag}{' ' + step.waypoint if step.waypoint else ''} -> "
                    f"{p.VERDICTS.get(v.verdict, 'none')} {p.RULES.get(v.rule, v.rule)} result {v.result}")
            seq = (seq + 1) & 0xFFFFFFFF
            if wait:
                c.pump(step.gap_ms / 1000.0 - (time.monotonic() - t0))
        if r.error is None:
            c.pump(TAIL_S if wait else 0.05)
    except ConnectionError:
        pass
    finally:
        c.close()
    return r


def check(r: RunResult, plan: list[Step]) -> list[str]:
    """What went wrong, if anything: denials, errors, unreached waypoints, SAFE_STOP."""
    problems: list[str] = []
    if r.error is not None:
        problems.append(f"the gateway sent ERROR {r.error}")
    if len(r.verdicts) != len(plan):
        problems.append(f"{len(r.verdicts)} of {len(plan)} commands answered")
    for v in r.verdicts:
        if v.result != p.DECIDED:
            problems.append(f"seq {v.seq}: not decided (result {v.result})")
        elif v.verdict != 1:
            problems.append(f"seq {v.seq}: {p.VERDICTS.get(v.verdict, v.verdict)} by rule "
                            f"{p.RULES.get(v.rule, v.rule)}")
    if any(s.state == 3 for s in r.states) or any(v.state == 3 for v in r.verdicts):
        problems.append("the guardian entered SAFE_STOP")
    # Each waypoint: some TELEMETRY frame at its target, from its arrival command
    # until the next walk starts (while the planner pauses there).
    starts = [i for i, s in enumerate(plan) if s.tag == "walk" and (i == 0 or plan[i - 1].tag != "walk")]
    for i, s in enumerate(plan):
        if s.waypoint is None or i >= len(r.verdicts):
            continue
        nxt = next((j for j in starts if j > i), len(plan))
        window = [t for after, t in r.telemetry if i < after <= nxt]
        dist = [max(abs(t.pos_urad[k] - s.target_urad[k]) for k in range(2)) for t in window]
        if not any(d <= REACHED_TOL_URAD for d in dist):
            problems.append(f"waypoint {s.waypoint} {s.target_urad} not reached in the telemetry: "
                            f"{len(window)} frames while the planner paused there (after commands {i + 1} "
                            f"to {nxt}), closest {min(dist) if dist else 'none'} µrad; "
                            f"{len(r.telemetry)} frames in all")
    return problems


def summary(r: RunResult, plan: list[Step], problems: list[str]) -> str:
    n = {name: sum(1 for v in r.verdicts if v.result == p.DECIDED and v.verdict == code)
         for code, name in p.VERDICTS.items()}
    reached = sum(1 for s in plan if s.waypoint) - sum(1 for x in problems if x.startswith("waypoint "))
    return (f"SUMMARY planner {len(r.verdicts)}/{len(plan)} commands answered: {n['ALLOW']} ALLOW, "
            f"{n['CLAMP']} CLAMP, {n['DENY']} DENY; {len(r.telemetry)} TELEMETRY frames; waypoints reached "
            f"{reached}/{sum(1 for s in plan if s.waypoint)}; {len(problems)} problem(s)")
