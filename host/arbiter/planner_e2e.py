"""`arbiter planner e2e --target <t>`: the scripted planner, on this host, through the
real gateway, guardian and arm on a QNX target (FR-P1, FR-N1, FR-N4).

On the target, qnx/tests/planner_e2e.sh starts the watchdog, the arm, the guardian,
the evidence logger and the gateway (each as its own type on the policy image),
then waits. Here the planner connects over TCP, as a planner on another machine
would, and plays the pick-and-place path. Then the target stops the system and
prints the evidence log, and this program judges:

  - every command ALLOW, every waypoint reached in the TELEMETRY the gateway
    relayed, never SAFE_STOP (`planner.check()`);
  - the target's evidence log verifies (hash chain and anchor, `arbiter evidence
    verify`) and holds a record for every verdict the planner received;
  - every trusted process is still running at the end.

The whole output is saved under evidence/runs/<date>-<target>/planner-e2e-<time>.log.
"""

from __future__ import annotations

import datetime as dt
import socket
import subprocess
import time
from pathlib import Path

from . import evidence
from . import planner as pl
from . import protocol as p
from .contract import compile_file

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_SSH = REPO_ROOT / "scripts" / "target-ssh.sh"
DEPLOY = REPO_ROOT / "scripts" / "deploy.sh"
REMOTE_CONTRACT = "planner/contract.bin"
READY_TIMEOUT_S = 30
TARGET_TIMEOUT_S = 180


def _ssh(target: str, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(TARGET_SSH), target, *args], capture_output=True, text=True,
                          timeout=TARGET_TIMEOUT_S, check=False)


def _git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO_ROOT), *args], capture_output=True, text=True,
                          check=False).stdout.strip()


def _wait_ready(host: str) -> bool:
    """The gateway is up when its observer port answers with a WELCOME (the planner port stays free)."""
    end = time.monotonic() + READY_TIMEOUT_S
    while time.monotonic() < end:
        try:
            with socket.create_connection((host, p.OBSERVER_PORT), timeout=2) as s:
                b = s.recv(p.FRAME_LEN, socket.MSG_WAITALL)
                if len(b) == p.FRAME_LEN and isinstance(p.decode(b), p.Welcome):
                    return True
        except (OSError, ValueError):
            pass
        time.sleep(0.5)
    return False


def fr_p1_scripted_planner_runs_pick_and_place_through_the_gateway(target: str, out_root: Path,
                                                                 deploy: bool = True) -> int:
    lines: list[str] = []

    def say(s: str) -> None:
        print(s, flush=True)
        lines.append(s)

    now = dt.datetime.now().astimezone()
    root = out_root if out_root.is_absolute() else REPO_ROOT / out_root
    out = root / f"{now:%Y-%m-%d}-{target}" / f"planner-e2e-{now:%H%M%S}.log"
    out.parent.mkdir(parents=True, exist_ok=True)
    dirty = _git("status", "--porcelain", "--", "qnx", "host", "scripts", "contracts")
    say(f"# planner e2e {target} — {now.isoformat(timespec='seconds')}")
    say(f"# commit: {_git('rev-parse', '--short', 'HEAD')}{' (UNCOMMITTED changes)' if dirty else ''}")

    table = REPO_ROOT / "build" / "planner-e2e" / "contract.bin"
    table.parent.mkdir(parents=True, exist_ok=True)
    table.write_bytes(compile_file(REPO_ROOT / "contracts" / "default.yaml").pack())
    if deploy:
        d = subprocess.run([str(DEPLOY), target], capture_output=True, text=True, check=False)
        if d.returncode != 0:
            say(f"FAIL deploy:\n{d.stdout}{d.stderr}")
            out.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return 1
    for args in (("put", str(table), REMOTE_CONTRACT),):
        r = _ssh(target, *args)
        if r.returncode != 0:
            say(f"FAIL cannot copy the contract: {r.stderr.strip()}")
            out.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return 1
    host = _ssh(target, "host").stdout.strip()
    say(f"# target {target} at {host}: {_ssh(target, 'run', 'uname -a').stdout.strip()}")

    # The target side runs in the background until told to stop.
    side = subprocess.Popen([str(TARGET_SSH), target, "run", f"./planner_e2e.sh {REMOTE_CONTRACT}"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    problems: list[str] = []
    result: pl.RunResult | None = None
    plan = pl.scripted_plan()
    try:
        if not _wait_ready(host):
            problems.append(f"the gateway at {host}:{p.OBSERVER_PORT} did not answer "
                            f"within {READY_TIMEOUT_S} s")
        else:
            say(f"## the scripted planner, from this host to {host}:{p.PLANNER_PORT}: {len(plan)} commands")
            result = pl.run(host, p.PLANNER_PORT, plan, log=say)
            problems += pl.check(result, plan)
    finally:
        _ssh(target, "run", "touch /tmp/arbiter-planner/done")
        try:
            target_out, _ = side.communicate(timeout=TARGET_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            side.kill()
            target_out, _ = side.communicate()
            problems.append("the target side did not finish")

    say("## the target")
    evlog: list[str] = []
    anchor = ""
    alive = ""
    for line in target_out.splitlines():
        if line.startswith("EVLOG "):
            evlog.append(line[len("EVLOG "):])
        else:
            say(line)
            if line.startswith("ANCHOR "):
                anchor = line.split()[1]
            if line.startswith("ALIVE "):
                alive = line
    say(f"## the evidence log: {len(evlog)} lines (kept below, prefixed EVLOG)")
    v = evidence.verify("".join(f"{x}\n" for x in evlog), anchor=anchor or None)
    if not v.ok:
        problems += [f"evidence log: {e}" for e in v.errors]
    if not v.terminated:
        problems.append("evidence log: no end record")
    logged = {int(x.split(" seq ")[1].split()[0]) for x in evlog if x.startswith("R ") and " seq " in x}
    if result is not None:
        missing = [vd.seq for vd in result.verdicts if vd.seq not in logged]
        if missing:
            problems.append(f"evidence log: no record for {len(missing)} verdicts, first seq {missing[0]}")
        say(f"evidence log verifies: {v.ok}, {v.records} decisions, {v.lost} declared lost; "
            f"every verdict logged: {not missing}")
    if alive != "ALIVE watchdog 1 arm 1 guardian 1 logd 1 gateway 1":
        problems.append(f"not every process ran to the end: '{alive}'")

    for x in problems:
        say(f"  problem: {x}")
    name = "fr_p1_scripted_planner_runs_pick_and_place_through_the_gateway"
    say(f"{'PASS' if not problems else 'FAIL'} {name} ({target})")
    lines += [f"EVLOG {x}" for x in evlog]
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"log: {out.relative_to(REPO_ROOT) if out.is_relative_to(REPO_ROOT) else out}")
    return 0 if not problems else 1
