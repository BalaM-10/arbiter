#!/usr/bin/env python3
"""The README's expected results must match the newest saved run (midpoint review M5).

Checks, against the newest policy-off QEMU run log in evidence/runs/*-qemu/:
  - the README's `SUMMARY <n>/<m> tests passed on qemu` equals the log's SUMMARY line;
  - the README's `RESULT PASS test_libcontract <n>/<m>` equals the log's;
and that the README's "Latest report: <n>/<m>" link points at a bench report that
exists and says n of m passed.

A failed newest run fails this check too: the README then promises a result
the evidence does not show. Run:  python3 scripts/check-readme.py
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


def newest_qemu_log() -> Path | None:
    logs = sorted(ROOT.glob("evidence/runs/*-qemu/run-*.log"), key=lambda p: (p.parent.name, p.name))
    return logs[-1] if logs else None


def nfr_k3_readme_expected_results_match_the_newest_run() -> list[str]:
    errors: list[str] = []
    readme = README.read_text(encoding="utf-8")
    log = newest_qemu_log()
    if log is None:
        return ["no run log under evidence/runs/*-qemu/"]
    text = log.read_text(encoding="utf-8", errors="replace")
    rel = log.relative_to(ROOT)
    for name, rx in (("SUMMARY", SUMMARY_RE), ("test_libcontract", LIBCONTRACT_RE)):
        want = rx.search(readme)
        got = rx.findall(text)
        if want is None:
            errors.append(f"README.md: no expected {name} line")
        elif not got:
            errors.append(f"{rel}: no passing {name} line (the README expects {want.group(1)}/{want.group(2)})")
        elif (want.group(1), want.group(2)) != got[-1]:
            errors.append(f"README.md expects {name} {want.group(1)}/{want.group(2)}, "
                          f"{rel} shows {got[-1][0]}/{got[-1][1]}")
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
