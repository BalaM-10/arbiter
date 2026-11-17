"""Verify an evidence log written by arb-logd (format: docs/EVIDENCE_LOG.md).

FR-E2: every line carries the SHA-256 of the previous one, and its own hash is
the SHA-256 of its text up to and including its `prev` value. FR-E4: any edit,
removed line, reorder or undeclared gap fails, naming the first bad line;
lines removed from the end are caught only against a published anchor (TM-C6).
FR-E5: losses the logger declared (G lines) pass and are counted.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field

ZERO = "0" * 64
HEX64 = re.compile(r"[0-9a-f]{64}")
INT = re.compile(r"-?[0-9]+")

# Keys of each line type, in order, with how many values each takes (docs/EVIDENCE_LOG.md).
SCHEMA: dict[str, list[tuple[str, int]]] = {
    "H": [("format", 1), ("contract", 1), ("cycles_per_sec", 1), ("start_ns", 1)],
    "R": [("idx", 1), ("t_ns", 1), ("seq", 1), ("target", 2), ("dur", 1), ("verdict", 1), ("rule", 1),
          ("before", 1), ("after", 1), ("flags", 1), ("fwd", 3), ("latency_cycles", 1)],
    "G": [("lost", 1), ("from_idx", 1), ("to_idx", 1)],
    "E": [("records", 1), ("lost", 1)],
}


def line_hash(text: str) -> str:
    return hashlib.sha256(text.encode("ascii")).hexdigest()


@dataclass
class Line:
    tag: str
    n: int
    values: dict[str, list[str]]
    prev: str
    hash: str
    text: str                       # everything before " hash "

    def int(self, key: str, i: int = 0) -> int:
        return int(self.values[key][i])


@dataclass
class Result:
    ok: bool = True
    errors: list[str] = field(default_factory=list)
    lines: int = 0
    records: int = 0
    lost: int = 0
    contract: str = ""
    terminated: bool = False
    last_hash: str = ""
    decisions: list[Line] = field(default_factory=list)   # the R lines, in order

    def fail(self, msg: str) -> None:
        self.ok = False
        self.errors.append(msg)


def parse_line(raw: str) -> Line:
    """One line, or ValueError saying what is wrong with it."""
    text, sep, h = raw.rpartition(" hash ")
    if not sep or not HEX64.fullmatch(h):
        raise ValueError("no ' hash <64 hex>' at the end")
    tok = text.split(" ")
    if len(tok) < 4 or tok[0] not in SCHEMA:
        raise ValueError("unknown line type")
    if not INT.fullmatch(tok[1]):
        raise ValueError("no line number")
    values: dict[str, list[str]] = {}
    i = 2
    for key, count in SCHEMA[tok[0]]:
        if i + count >= len(tok) + 1 or tok[i] != key:
            raise ValueError(f"expected key '{key}'")
        vals = tok[i + 1:i + 1 + count]
        if len(vals) != count:
            raise ValueError(f"'{key}' needs {count} value(s)")
        for v in vals:
            if not (HEX64.fullmatch(v) if key == "contract" else INT.fullmatch(v)):
                raise ValueError(f"bad value for '{key}': {v}")
        values[key] = vals
        i += 1 + count
    if tok[i:] != ["prev", tok[-1]] or not HEX64.fullmatch(tok[-1]):
        raise ValueError("expected 'prev <64 hex>' after the fields")
    return Line(tok[0], int(tok[1]), values, tok[-1], h, text)


def verify(log_text: str, anchor: str | None = None) -> Result:
    r = Result()
    prev = ZERO
    next_idx = 0
    raw_lines = log_text.split("\n")
    if raw_lines and raw_lines[-1] == "":
        raw_lines.pop()                                    # the final newline
    for k, raw in enumerate(raw_lines):
        where = f"line {k}"
        try:
            ln = parse_line(raw)
        except ValueError as e:
            r.fail(f"{where}: {e}: {raw[:100]}")
            return r
        if ln.n != k:
            r.fail(f"{where}: numbered {ln.n} (a line was removed, added or moved)")
            return r
        if ln.prev != prev:
            r.fail(f"{where}: prev is not the hash of line {k - 1} (a line was removed, edited or moved)")
            return r
        if line_hash(ln.text) != ln.hash:
            r.fail(f"{where}: hash does not match its text (the line was edited)")
            return r
        if (k == 0) != (ln.tag == "H"):
            r.fail(f"{where}: the header must be line 0, and only line 0")
            return r
        if r.terminated:
            r.fail(f"{where}: a line after the end record")
            return r
        if ln.tag == "H":
            r.contract = ln.values["contract"][0]
        elif ln.tag == "G":
            a, b, lost = ln.int("from_idx"), ln.int("to_idx"), ln.int("lost")
            if a != next_idx or b < a or lost != b - a + 1:
                r.fail(f"{where}: gap {a}..{b} ({lost} lost) does not follow decision {next_idx - 1}")
                return r
            r.lost += lost
            next_idx = b + 1
        elif ln.tag == "R":
            if ln.int("idx") != next_idx:
                r.fail(f"{where}: decision idx {ln.int('idx')}, expected {next_idx} (an undeclared gap)")
                return r
            next_idx += 1
            r.records += 1
            r.decisions.append(ln)
        elif ln.tag == "E":
            if ln.int("records") != r.records or ln.int("lost") != r.lost:
                r.fail(f"{where}: end record says {ln.int('records')} records, {ln.int('lost')} lost; "
                       f"the log has {r.records} and {r.lost}")
                return r
            r.terminated = True
        prev = ln.hash
        r.lines += 1
    r.last_hash = prev if r.lines else ""
    if r.lines == 0:
        r.fail("line 0: empty log")
    elif anchor is not None and anchor.strip() != r.last_hash:
        r.fail(f"line {r.lines - 1}: last hash differs from the anchor "
               "(lines removed from the end, or another log)")
    return r
