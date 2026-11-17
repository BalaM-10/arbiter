"""The evidence packet (FR-E3, FR-C6): one fault-bench folder made checkable as a whole.

`arbiter bench run` writes `packet.json` next to its report. It holds:

  - the contract's SHA-256 (the hash of its source, FR-C3), which every
    scenario's evidence log must name in its header (FR-C6);
  - the build identifiers: the SHA-256 of each Arbiter binary as the target hashed
    it (`arb-logd --sha256`, the QNX Cryptography Library), and of the same file
    in the host's build folder;
  - the target's description: its name, `uname -a`, `pidin info`, and the security
    policy loaded (its id, or none);
  - the bench results: passed and total, and each scenario's outcome;
  - each scenario's evidence log, with its anchor and record count;
  - the SHA-256 of every file in the folder.

`verify()` (`arbiter evidence verify-packet <dir>`) recomputes all of it: every
file's hash, every log's chain from its anchor, every log's contract against the
packet's, the target's binaries against the build, and the results against
report.json. Anyone who can rewrite the whole folder and packet.json can forge
it (THREAT_MODEL O1): the packet detects edits, not a forger with write access to
everything.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import evidence

FORMAT = 1
NAME = "packet.json"


def _sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def _files(d: Path) -> dict[str, str]:
    return {str(f.relative_to(d)): _sha256(f) for f in sorted(d.rglob("*")) if f.is_file() and f.name != NAME}


def write(d: Path, *, target: dict[str, str], builds: dict[str, Any]) -> Path:
    """Write d/packet.json for the bench folder d (report.json and raw/ already written)."""
    rep = json.loads((d / "report.json").read_text(encoding="utf-8"))
    logs = []
    for sc in rep["scenarios"]:
        log = d / sc["evidence"]
        anchor = log.with_name(log.name.replace(".evidence.log", ".evidence.anchor"))
        text = log.read_text(encoding="ascii", errors="replace") if log.is_file() else ""
        a = anchor.read_text(encoding="ascii").strip() if anchor.is_file() else ""
        v = evidence.verify(text, anchor=a or None)
        logs.append({"scenario": sc["id"], "log": sc["evidence"], "anchor": a, "records": v.records,
                     "lost": v.lost, "contract": v.contract})
    packet = {
        "format": FORMAT,
        "created": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "commit": rep["meta"].get("commit", ""),
        "contract_sha256": rep["meta"]["contract_sha256"],
        "target": target,
        "builds": builds,
        "bench": {"passed": rep["passed"], "total": rep["total"],
                  "scenarios": {sc["id"]: bool(sc["passed"]) for sc in rep["scenarios"]}},
        "logs": logs,
        "files": _files(d),
    }
    out = d / NAME
    out.write_text(json.dumps(packet, indent=1) + "\n", encoding="utf-8")
    return out


@dataclass
class Result:
    errors: list[str] = field(default_factory=list)
    checked: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.errors


def verify(d: Path) -> Result:
    r = Result()
    try:
        p = json.loads((d / NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError) as err:
        r.errors.append(f"{NAME}: {err}")
        return r
    if p.get("format") != FORMAT:
        r.errors.append(f"{NAME}: format {p.get('format')}, expected {FORMAT}")
        return r
    # Every file, as listed; nothing missing, nothing added.
    now = _files(d)
    for rel, h in p["files"].items():
        if rel not in now:
            r.errors.append(f"{rel}: missing")
        elif now[rel] != h:
            r.errors.append(f"{rel}: changed (sha256 {now[rel][:16]}..., packet says {h[:16]}...)")
    for rel in sorted(set(now) - set(p["files"])):
        r.errors.append(f"{rel}: not in the packet")
    r.checked.append(f"{len(p['files'])} files hashed")
    # Each log's chain from its anchor, and its contract (FR-C6).
    contract = p["contract_sha256"]
    for x in p["logs"]:
        log = d / x["log"]
        if not log.is_file():
            continue                                  # reported above as missing
        v = evidence.verify(log.read_text(encoding="ascii", errors="replace"), anchor=x["anchor"] or None)
        if not v.ok:
            r.errors += [f"{x['scenario']} evidence log: {e}" for e in v.errors]
        if not x["anchor"]:
            r.errors.append(f"{x['scenario']} evidence log: no anchor")
        if v.contract != contract:
            r.errors.append(f"{x['scenario']} evidence log names contract {v.contract}, "
                            f"the packet {contract}")
    r.checked.append(f"{len(p['logs'])} evidence logs verified against their anchors and the contract")
    # The binaries the target ran are the build's.
    b = p["builds"]
    for name, h in sorted(b.get("host", {}).items()):
        t = b.get("target", {}).get(name)
        if t != h:
            r.errors.append(f"binary {name}: on the target {t}, in the build {h}")
    r.checked.append(f"{len(b.get('host', {}))} binaries: target = build")
    # The results are the report's.
    try:
        rep = json.loads((d / "report.json").read_text(encoding="utf-8"))
        res = {"passed": rep["passed"], "total": rep["total"],
               "scenarios": {sc["id"]: bool(sc["passed"]) for sc in rep["scenarios"]}}
        if res != p["bench"]:
            r.errors.append("report.json: results differ from the packet's")
        if rep["meta"].get("contract_sha256") != contract:
            r.errors.append("report.json: names another contract")
    except (OSError, ValueError, KeyError) as err:
        r.errors.append(f"report.json: {err}")
    return r
