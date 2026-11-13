#!/usr/bin/env python3
"""Every "cannot bypass"-style claim in the public docs must cite docs/THREAT_MODEL.md.

A line in README.md or docs/*.md that says an attacker or untrusted party
*cannot* do something, or that only the guardian can, must contain a claim ID
(TM-C1 ... TM-C7) that exists in docs/THREAT_MODEL.md. A line that states a
limitation rather than a protection can be marked with `<!-- not a claim -->`.

Only files that would be published (tracked or new, not ignored) are checked.
Run:  python3 scripts/check-claims.py
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THREAT_MODEL = ROOT / "docs" / "THREAT_MODEL.md"
CLAIM_RE = re.compile(
    r"\bcannot\b|\bbypass\w*|\bonly the guardian\b|\bonly process\b|\bno untrusted\b", re.IGNORECASE)
REF_RE = re.compile(r"\bTM-C\d+\b")
EXEMPT = "<!-- not a claim -->"


def published_docs() -> list[Path]:
    out = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-co", "--exclude-standard", "--", "README.md", "docs"],
        capture_output=True, text=True, check=True).stdout.split()
    return [ROOT / p for p in out if p.endswith(".md") and (ROOT / p).is_file()]


def main() -> int:
    known = set(REF_RE.findall(THREAT_MODEL.read_text(encoding="utf-8")))
    errors: list[str] = []
    for path in published_docs():
        if path == THREAT_MODEL:
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if not CLAIM_RE.search(line) or EXEMPT in line:
                continue
            refs = set(REF_RE.findall(line))
            rel = path.relative_to(ROOT)
            if not refs:
                errors.append(f"{rel}:{n}: claim without a TM-C reference: {line.strip()[:120]}")
            for r in sorted(refs - known):
                errors.append(f"{rel}:{n}: {r} is not a claim in docs/THREAT_MODEL.md")
    for e in errors:
        print(f"FAIL {e}")
    print(f"RESULT {'FAIL' if errors else 'PASS'} check-claims: {len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
