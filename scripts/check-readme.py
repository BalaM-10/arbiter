#!/usr/bin/env python3
"""The README's expected results must match the newest saved run (midpoint review M5).

Checks, against the policy-off QEMU run logs in evidence/runs/*-qemu/:
  - the README's `SUMMARY <n>/<m> tests passed on qemu`: m equals the newest
    log's total (a suite was added or removed: update the README), and n/m equals
    the newest fully passing log's summary;
  - the README's `RESULT PASS test_libcontract <n>/<m>` equals the newest log that
    has that line;
and that the README's "Latest report: <n>/<m>" link points at a bench report that
exists and says n of m passed.

A log saved on purpose before a fix (the tests fail first) does not fail this
check, but the README can never promise a count that no saved run shows.
Run:  python3 scripts/check-readme.py
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
SUMMARY_RE = re.compile(r"SUMMARY (\d+)/(\d+) tests passed on qemu\b")
LIBCONTRACT_RE = re.compile(r"RESULT PASS test_libcontract (\d+)/(\d+)")
BENCH_LINK_RE = re.compile(r"\[Latest report: (\d+)/(\d+)\]\((evidence/bench/[^)]+/report\.html)\)")


def qemu_logs() -> list[Path]:
    """Oldest first."""
    return sorted(ROOT.glob("evidence/runs/*-qemu/run-*.log"), key=lambda p: (p.parent.name, p.name))


def newest_qemu_log() -> Path | None:
    logs = qemu_logs()
    return logs[-1] if logs else None


def _text(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def nfr_k3_readme_expected_results_match_the_newest_run() -> list[str]:
    errors: list[str] = []
    readme = README.read_text(encoding="utf-8")
    log = newest_qemu_log()
    if log is None:
        return ["no run log under evidence/runs/*-qemu/"]
    rel = log.relative_to(ROOT)
    newest = SUMMARY_RE.findall(_text(log))
    passing = [(p, m[-1]) for p in qemu_logs()
               if (m := SUMMARY_RE.findall(_text(p))) and m[-1][0] == m[-1][1]]
    want = SUMMARY_RE.search(readme)
    if want is None:
        errors.append("README.md: no expected SUMMARY line")
    else:
        if not newest:
            errors.append(f"{rel}: no SUMMARY line")
        elif newest[-1][1] != want.group(2):
            errors.append(f"README.md expects {want.group(2)} tests, {rel} ran {newest[-1][1]}")
        if not passing:
            errors.append("no fully passing QEMU run log")
        elif passing[-1][1] != (want.group(1), want.group(2)):
            path, (n, m) = passing[-1]
            errors.append(f"README.md expects SUMMARY {want.group(1)}/{want.group(2)}, "
                          f"the newest passing run {path.relative_to(ROOT)} shows {n}/{m}")
    want = LIBCONTRACT_RE.search(readme)
    have = [m[-1] for p in qemu_logs() if (m := LIBCONTRACT_RE.findall(_text(p)))]
    if want is None:
        errors.append("README.md: no expected test_libcontract line")
    elif not have:
        errors.append("no run log with a passing test_libcontract line")
    elif have[-1] != (want.group(1), want.group(2)):
        errors.append(f"README.md expects test_libcontract {want.group(1)}/{want.group(2)}, "
                      f"the newest run shows {have[-1][0]}/{have[-1][1]}")
    link = BENCH_LINK_RE.search(readme)
    if link is None:
        errors.append("README.md: no 'Latest report: n/m' bench link")
    else:
        report = ROOT / link.group(3).replace("report.html", "report.json")
        if not report.is_file():
            errors.append(f"README.md links {link.group(3)}, which has no report.json")
        else:
            rep = json.loads(report.read_text(encoding="utf-8"))
            if (str(rep.get("passed")), str(rep.get("total"))) != (link.group(1), link.group(2)):
                errors.append(f"README.md says {link.group(1)}/{link.group(2)}, "
                              f"{link.group(3)} says {rep.get('passed')}/{rep.get('total')}")
    return errors


def main() -> int:
    errors = nfr_k3_readme_expected_results_match_the_newest_run()
    for e in errors:
        print(f"FAIL {e}")
    log = newest_qemu_log()
    print(f"RESULT {'PASS' if not errors else 'FAIL'} check-readme: newest run "
          f"{log.relative_to(ROOT) if log else 'none'}, {len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
