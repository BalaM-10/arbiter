"""`arbiter bench run`: deploy, run each scenario on the target, judge, report (FR-B1..B3)."""

from __future__ import annotations

import datetime as dt
import gzip
import subprocess
from pathlib import Path
from typing import Any

from ..contract import compile_file
from . import report
from .checks import TIMING_POLICY, Check, timing_judged
from .records import parse
from .scenarios import ALL
from .spec import REPO_ROOT, load_spec

TARGET_SSH = REPO_ROOT / "scripts" / "target-ssh.sh"
DEPLOY = REPO_ROOT / "scripts" / "deploy.sh"
REMOTE_CONTRACT = "bench/contract.bin"
SCENARIO_TIMEOUT_S = 180


def _ssh(target: str, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(TARGET_SSH), target, *args], input=stdin, capture_output=True, text=True,
                          timeout=SCENARIO_TIMEOUT_S, check=False)


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
        print(f"{sid} {mod.TITLE}: {len(script.commands)} commands, {script.end_ms / 1000:.1f} s ...",
              flush=True)
        r = _ssh(target, "run", f"./bench_scenario.sh {REMOTE_CONTRACT}", stdin=text)
        (out / "raw" / f"{sid}.script").write_text(text, encoding="utf-8")
        with gzip.open(out / "raw" / f"{sid}.out.gz", "wt", encoding="utf-8") as f:
            f.write(r.stdout)
            if r.stderr:
                f.write("".join(f"STDERR {line}\n" for line in r.stderr.splitlines()))
        run = parse(r.stdout, r.returncode)
        run.target = target
        # This scenario's system log (SLOG lines from bench_scenario.sh), kept on its own.
        slog_text = "".join(f"{line}\n" for line in run.slog)
        (out / "raw" / f"{sid}.slog2.txt").write_text(slog_text, encoding="utf-8")
        checks: list[Check] = mod.judge(run, script, spec)
        passed = all(c.passed for c in checks)
        for c in checks:
            print(f"  {'pass' if c.passed else 'FAIL'} {c.name}: {c.detail}")
        print(f"{sid} {'PASS' if passed else 'FAIL'}", flush=True)
        scenarios.append({
            "id": sid, "title": mod.TITLE, "pass_condition": mod.PASS_CONDITION, "passed": passed,
            "checks": [{"name": c.name, "passed": c.passed, "detail": c.detail, "data": c.data}
                       for c in checks],
            "samples": len(run.samples), "commands": len(script.commands),
            "raw": f"raw/{sid}.out.gz", "script": f"raw/{sid}.script", "slog2": f"raw/{sid}.slog2.txt",
            "paths": report.tip_paths(run)})

    n_pass = sum(1 for s in scenarios if s["passed"])
    rep = {"meta": meta, "passed": n_pass, "total": len(scenarios), "scenarios": scenarios}
    report.write(rep, spec, out)
    print(f"SUMMARY bench {n_pass}/{len(scenarios)} scenarios passed on {target}")
    page = out / "report.html"
    print(f"report: {page.relative_to(REPO_ROOT) if page.is_relative_to(REPO_ROOT) else page}")
    return 0 if n_pass == len(scenarios) else 1
