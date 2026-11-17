"""`arbiter bench run`: deploy, run each scenario on the target, judge, report (FR-B1..B3)."""

from __future__ import annotations

import datetime as dt
import gzip
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from .. import packet
from .. import planner as pl
from .. import protocol as pr
from ..contract import compile_file
from . import checks as ck
from . import report
from . import transport as tr
from .checks import TIMING_POLICY, Check, timing_judged
from .records import parse
from .scenarios import ALL
from .spec import REPO_ROOT, load_spec

TARGET_SSH = REPO_ROOT / "scripts" / "target-ssh.sh"
DEPLOY = REPO_ROOT / "scripts" / "deploy.sh"
REMOTE_CONTRACT = "bench/contract.bin"
SCENARIO_TIMEOUT_S = 180
GATEWAY_READY_S = 30     # the target starts four processes before the gateway


def _ssh(target: str, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(TARGET_SSH), target, *args], input=stdin, capture_output=True, text=True,
                          timeout=SCENARIO_TIMEOUT_S, check=False)


def _run_scenario(target: str, mod: Any, script: Any) -> tuple[str, int, pl.RunResult | None]:
    """Run one scenario on the target; returns its output, exit status and, for a
    scenario played through the gateway (D6), what the planner received."""
    if getattr(mod, "TRANSPORT", "inject") != "gateway":
        r = _ssh(target, "run", f"./bench_scenario.sh {REMOTE_CONTRACT}", stdin=script.text())
        return r.stdout + "".join(f"STDERR {x}\n" for x in r.stderr.splitlines()), r.returncode, None
    host = _ssh(target, "host").stdout.strip()
    cmd = f"./bench_scenario.sh {REMOTE_CONTRACT} gateway"
    with tempfile.TemporaryFile("w+", encoding="ascii") as rec:
        rec.write(tr.record_only_script(script))
        rec.seek(0)
        side = subprocess.Popen([str(TARGET_SSH), target, "run", cmd],
                                stdin=rec, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    result: pl.RunResult | None = None
    if pl.wait_ready(host, GATEWAY_READY_S):
        result = pl.run(host, pr.PLANNER_PORT, tr.script_steps(script))
    try:
        out, err = side.communicate(timeout=SCENARIO_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        side.kill()
        out, err = side.communicate()
    if result is None:
        err += "the gateway did not answer in time: no command was sent\n"
    elif result.error is not None:
        err += f"the gateway sent ERROR {result.error}\n"
    return out + "".join(f"STDERR {x}\n" for x in err.splitlines()), side.returncode, result


# The binaries whose identity goes into the evidence packet (FR-E3).
PACKET_BINARIES = ("arb-guardian", "arb-arm", "arb-watchdog", "arb-logd", "arb-gateway", "arb-inject",
                   "arb-ctl")


def _packet_inputs(target: str, uname: str) -> tuple[dict[str, str], dict[str, Any]]:
    """The target's description and the build identifiers, as the target reports them."""
    import hashlib

    info = _ssh(target, "info").stdout.split()
    arch = info[0] if info else ""
    pidin = _ssh(target, "run", "pidin info | head -4").stdout.strip()
    pol = _ssh(target, "run", "secpol -f all 2>/dev/null | head -1").stdout.strip()
    desc = {"name": target, "uname": uname, "pidin_info": pidin,
            "policy": pol if pol.startswith("policy_id:") else "none"}
    on_target: dict[str, str] = {}
    hashed = _ssh(target, "run", "./arb-logd --sha256 " + " ".join(PACKET_BINARIES)).stdout
    for line in hashed.splitlines():
        f = line.split()
        if len(f) == 2 and len(f[0]) == 64:
            on_target[f[1]] = f[0]
    host: dict[str, str] = {}
    for name in PACKET_BINARIES:
        b = REPO_ROOT / "build" / arch / name
        host[name] = hashlib.sha256(b.read_bytes()).hexdigest() if arch and b.is_file() else "missing"
    return desc, {"arch": arch, "target": on_target, "host": host}


def _git(*args: str) -> str:
    r = subprocess.run(["git", "-C", str(REPO_ROOT), *args], capture_output=True, text=True, check=False)
    return r.stdout.strip()


def run_bench(target: str, contract: Path, only: list[str] | None, out_root: Path, deploy: bool) -> int:
    spec = load_spec(contract)
    ids = only or list(ALL)
    unknown = [i for i in ids if i not in ALL]
    if unknown:
        raise SystemExit(f"unknown scenario(s) {unknown}; available: {list(ALL)}")

    table = REPO_ROOT / "build" / "bench" / "contract.bin"
    table.parent.mkdir(parents=True, exist_ok=True)
    table.write_bytes(compile_file(contract).pack())
    if deploy:
        d = subprocess.run([str(DEPLOY), target], capture_output=True, text=True, check=False)
        if d.returncode != 0:
            raise SystemExit(f"deploy failed:\n{d.stdout}{d.stderr}")
    p = _ssh(target, "put", str(table), REMOTE_CONTRACT)
    if p.returncode != 0:
        raise SystemExit(f"cannot copy the contract to {target}:\n{p.stderr}")
    uname = _ssh(target, "run", "uname -a").stdout.strip()

    now = dt.datetime.now().astimezone()
    root = out_root if out_root.is_absolute() else REPO_ROOT / out_root
    out = root / f"{now:%Y-%m-%d}-{target}" / f"{now:%H%M%S}"
    (out / "raw").mkdir(parents=True, exist_ok=True)
    dirty = _git("status", "--porcelain", "--", "qnx", "host", "scripts", "contracts")
    meta = {"started": now.isoformat(timespec="seconds"), "target": target, "uname": uname,
            "commit": _git("rev-parse", "--short", "HEAD") + (" (uncommitted changes)" if dirty else ""),
            "contract": str(contract.resolve().relative_to(REPO_ROOT)), "contract_sha256": spec.sha256,
            "tick_spacing_judged": timing_judged(target),
            "timing_policy": "tick spacing judged" if timing_judged(target) else TIMING_POLICY}

    scenarios: list[dict[str, Any]] = []
    for sid in ids:
        mod = ALL[sid]
        script = mod.build(spec)
        text = script.text()
        via = "planner -> gateway" if getattr(mod, "TRANSPORT", "inject") == "gateway" else "arb-inject"
        print(f"{sid} {mod.TITLE}: {len(script.commands)} commands, {script.end_ms / 1000:.1f} s, "
              f"via {via} ...", flush=True)
        stdout, rc, played = _run_scenario(target, mod, script)
        (out / "raw" / f"{sid}.script").write_text(text, encoding="utf-8")
        with gzip.open(out / "raw" / f"{sid}.out.gz", "wt", encoding="utf-8") as f:
            f.write(stdout)
        run = parse(stdout, rc)
        run.target = target
        transport = getattr(mod, "TRANSPORT", "inject")
        if played is not None:
            # D6: the verdicts the planner received, at the guardian's own decision
            # times (its evidence log), so the windows sit on the arm's samples.
            run.verdicts = tr.verdicts_from(played, run.evlog)
            decided = [v.t_send_ns for v in run.verdicts if v.rc == 0]
            run.t0_ns = decided[0] if decided else 0
            with gzip.open(out / "raw" / f"{sid}.planner.gz", "wt", encoding="utf-8") as f:
                for v in played.verdicts:
                    f.write(f"VERDICT seq {v.seq} result {v.result} verdict {v.verdict} rule {v.rule} "
                            f"state {v.state} fwd {v.fwd_urad[0]} {v.fwd_urad[1]} {v.fwd_duration_ms}\n")
                f.write(f"TELEMETRY frames {len(played.telemetry)}; STATE frames {len(played.states)}; "
                        f"error {played.error}\n")
        # This scenario's system log (SLOG lines from bench_scenario.sh), kept on its own.
        slog_text = "".join(f"{line}\n" for line in run.slog)
        (out / "raw" / f"{sid}.slog2.txt").write_text(slog_text, encoding="utf-8")
        checks: list[Check] = mod.judge(run, script, spec)
        # Every scenario: the target's evidence log (FR-E1, FR-E2, FR-E4, FR-C6).
        checks += [ck.evidence_log_verifies(run, spec), ck.evidence_log_has_every_verdict(run)]
        ev_text = "".join(f"{x}\n" for x in run.evlog)
        (out / "raw" / f"{sid}.evidence.log").write_text(ev_text, encoding="ascii")
        (out / "raw" / f"{sid}.evidence.anchor").write_text(f"{run.evanchor}\n", encoding="ascii")
        passed = all(c.passed for c in checks)
        for c in checks:
            print(f"  {'pass' if c.passed else 'FAIL'} {c.name}: {c.detail}")
        print(f"{sid} {'PASS' if passed else 'FAIL'}", flush=True)
        scenarios.append({
            "id": sid, "title": mod.TITLE, "pass_condition": mod.PASS_CONDITION, "passed": passed,
            "checks": [{"name": c.name, "passed": c.passed, "detail": c.detail, "data": c.data}
                       for c in checks],
            "samples": len(run.samples), "commands": len(script.commands),
            "transport": "planner -> gateway" if transport == "gateway" else "arb-inject",
            "raw": f"raw/{sid}.out.gz", "script": f"raw/{sid}.script", "slog2": f"raw/{sid}.slog2.txt",
            "evidence": f"raw/{sid}.evidence.log",
            "paths": report.tip_paths(run)})

    n_pass = sum(1 for s in scenarios if s["passed"])
    rep = {"meta": meta, "passed": n_pass, "total": len(scenarios), "scenarios": scenarios}
    report.write(rep, spec, out)
    print(f"SUMMARY bench {n_pass}/{len(scenarios)} scenarios passed on {target}")
    page = out / "report.html"
    print(f"report: {page.relative_to(REPO_ROOT) if page.is_relative_to(REPO_ROOT) else page}")
    # FR-E3: the evidence packet, written and checked at once.
    desc, builds = _packet_inputs(target, uname)
    packet.write(out, target=desc, builds=builds)
    pv = packet.verify(out)
    for e in pv.errors:
        print(f"  packet: {e}")
    pj = out / packet.NAME
    shown = pj.relative_to(REPO_ROOT) if pj.is_relative_to(REPO_ROOT) else pj
    print(f"packet: {'PASS' if pv.ok else 'FAIL'} {shown}{': ' + '; '.join(pv.checked) if pv.ok else ''}")
    return 0 if n_pass == len(scenarios) and pv.ok else 1
