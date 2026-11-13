#!/usr/bin/env python3
"""Check docs/TRACEABILITY.md against the requirements and the test sources.

Fails (exit 1) when:
  - a requirement ID in docs/REQUIREMENTS_SUMMARY.md has no row, or has more than one,
  - a row names an ID that is not a requirement,
  - a row's Test cell names a test that does not exist,
  - a test function named after a requirement (e.g. fr_g4_...) is not listed
    under that requirement, or is listed under a different one,
  - a row says it passes on a target but names no test.

Test references have the form `path::name`. For C and Python files, `name`
must be a function defined in that file; for other files, `name` must appear
in the file.

Run:  python3 scripts/check-traceability.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUMMARY = ROOT / "docs" / "REQUIREMENTS_SUMMARY.md"
TRACE = ROOT / "docs" / "TRACEABILITY.md"
TEST_GLOBS = ("qnx/tests/*.c", "host/tests/**/*.py")

ID_RE = re.compile(r"^(HC-\d+|FR-[A-Z]\d+|NFR-[A-Z]\d+|F\d+)$")
REF_RE = re.compile(r"`([^`:]+)::([A-Za-z0-9_]+)`")
C_FUNC_RE = re.compile(r"^\s*(?:static\s+)?(?:void|int)\s+([a-z][a-z0-9_]*)\s*\(", re.MULTILINE)
PY_FUNC_RE = re.compile(r"^\s*def\s+([a-z][a-z0-9_]*)\s*\(", re.MULTILINE)
TEST_NAME_RE = re.compile(r"^(hc|fr|nfr)_([a-z]?\d+)_|^(f)(\d+)_")


def table_rows(path: Path) -> list[list[str]]:
    """Cells of every markdown table row whose first cell is a requirement ID."""
    rows: list[list[str]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if cells and ID_RE.match(cells[0]):
            rows.append(cells)
    return rows


def id_for_test(name: str) -> str | None:
    """fr_g4_joint_limit -> FR-G4, nfr_i6_x -> NFR-I6, f5_x -> F5, else None."""
    m = TEST_NAME_RE.match(name)
    if not m:
        return None
    if m.group(1):
        prefix, num = m.group(1), m.group(2)
        letter, digits = (num[0], num[1:]) if num[0].isalpha() else ("", num)
        return f"{prefix.upper()}-{letter.upper()}{digits}"
    return f"F{m.group(4)}"


def defined_tests(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    regex = PY_FUNC_RE if path.suffix == ".py" else C_FUNC_RE
    return set(regex.findall(text))


def main() -> int:
    errors: list[str] = []

    required = [r[0] for r in table_rows(SUMMARY)]
    if len(set(required)) != len(required):
        errors.append("REQUIREMENTS_SUMMARY.md lists an ID more than once")
    required_set = set(required)

    rows = table_rows(TRACE)
    seen: dict[str, list[str]] = {}
    for cells in rows:
        if len(cells) < 5:
            errors.append(f"{cells[0]}: row has {len(cells)} cells, expected at least 5")
            continue
        rid = cells[0]
        if rid in seen:
            errors.append(f"{rid}: more than one row")
        seen[rid] = cells
        if rid not in required_set:
            errors.append(f"{rid}: not a requirement in REQUIREMENTS_SUMMARY.md")

    for rid in required:
        if rid not in seen:
            errors.append(f"{rid}: no row in TRACEABILITY.md")

    # Every referenced test must exist and sit under the requirement it names.
    listed: dict[str, str] = {}
    for rid, cells in seen.items():
        refs = REF_RE.findall(cells[2])
        for rel, name in refs:
            path = ROOT / rel
            if not path.is_file():
                errors.append(f"{rid}: test file {rel} does not exist")
                continue
            if path.suffix in (".c", ".py"):
                if name not in defined_tests(path):
                    errors.append(f"{rid}: {rel} defines no test named {name}")
            elif name not in path.read_text(encoding="utf-8"):
                errors.append(f"{rid}: {rel} does not contain {name}")
            owner = id_for_test(name)
            if owner is not None and owner != rid:
                errors.append(f"{rid}: test {name} is named for {owner}")
            listed[name] = rid
        passes = (cells[3] + " " + cells[4]).lower()
        if "yes" in passes and not refs:
            errors.append(f"{rid}: marked as passing on a target but names no test")

    # Every requirement-named test in the test sources must be listed.
    for pattern in TEST_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            for name in sorted(defined_tests(path)):
                owner = id_for_test(name)
                if owner is None:
                    continue
                if owner not in required_set:
                    errors.append(f"{path.relative_to(ROOT)}::{name}: names unknown requirement {owner}")
                elif name not in listed:
                    errors.append(f"{path.relative_to(ROOT)}::{name}: not listed under {owner}")

    if errors:
        for e in errors:
            print(f"FAIL {e}")
        print(f"RESULT FAIL check-traceability: {len(errors)} problem(s)")
        return 1
    tested = sum(1 for cells in seen.values() if REF_RE.search(cells[2]))
    print(f"RESULT PASS check-traceability: {len(required)} requirements, "
          f"{tested} with tests, {len(listed)} test references")
    return 0


if __name__ == "__main__":
    sys.exit(main())
