"""Host tests of the evidence packet (FR-E3, FR-C6): `arbiter.packet`.

A packet is a fault-bench folder plus `packet.json`: the contract hash, the
build identifiers (each binary's SHA-256 on the target and in the host's build),
the target's description, the bench results and every scenario's evidence log
with its anchor, and the SHA-256 of every file. `verify()` recomputes all of it.
The logs here are hashed by this file's own writer (docs/EVIDENCE_LOG.md).
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from arbiter import packet as pk
from arbiter.cli import main as cli_main

CONTRACT = "cd" * 32


def _log(seqs: list[int], contract: str = CONTRACT) -> tuple[str, str]:
    bodies = [f"H format 1 contract {contract} cycles_per_sec 1 start_ns 1"]
    for i, s in enumerate(seqs):
        bodies.append(f"R idx {i} t_ns {i + 1} seq {s} target 0 0 dur 100 verdict 1 rule 0 before 2 after 2 "
                      f"flags 0 fwd 0 0 100 latency_cycles 9")
    bodies.append(f"E records {len(seqs)} lost 0")
    lines, prev = [], "0" * 64
    for n, body in enumerate(bodies):
        tag, _, rest = body.partition(" ")
        text = f"{tag} {n} {rest} prev {prev}"
        prev = hashlib.sha256(text.encode()).hexdigest()
        lines.append(f"{text} hash {prev}")
    return "\n".join(lines) + "\n", prev


def _bench(tmp: Path, contract: str = CONTRACT) -> Path:
    d = tmp / "2026-11-17-qemu" / "120000"
    (d / "raw").mkdir(parents=True)
    scen = []
    for sid, seqs in (("F1", [1, 2, 3]), ("F9", [1, 1])):
        text, anchor = _log(seqs, contract)
        (d / "raw" / f"{sid}.evidence.log").write_text(text, encoding="ascii")
        (d / "raw" / f"{sid}.evidence.anchor").write_text(anchor + "\n", encoding="ascii")
        scen.append({"id": sid, "title": sid, "passed": True, "evidence": f"raw/{sid}.evidence.log"})
    rep = {"meta": {"target": "qemu", "commit": "abc1234", "contract_sha256": contract},
           "passed": 2, "total": 2, "scenarios": scen}
    (d / "report.json").write_text(json.dumps(rep), encoding="utf-8")
    (d / "report.html").write_text("<html></html>", encoding="utf-8")
    return d


def _write(d: Path, target_bin: dict[str, str] | None = None) -> None:
    host_bin = {"arb-guardian": "11" * 32, "arb-arm": "22" * 32}
    on_target = target_bin if target_bin is not None else host_bin
    pk.write(d, target={"name": "qemu", "uname": "QNX x 8.0.0", "pidin_info": "CPU:X86_64", "policy": "none"},
             builds={"arch": "x86_64", "host": host_bin, "target": on_target})


def fr_e3_packet_bundles_logs_contract_builds_target_and_bench_results(tmp_path: Path) -> None:
    d = _bench(tmp_path)
    _write(d)
    p = json.loads((d / "packet.json").read_text(encoding="utf-8"))
    assert p["format"] == 1
    assert p["contract_sha256"] == CONTRACT
    assert p["target"]["uname"] == "QNX x 8.0.0" and p["target"]["policy"] == "none"
    assert p["builds"]["target"]["arb-guardian"] == "11" * 32
    assert p["bench"] == {"passed": 2, "total": 2, "scenarios": {"F1": True, "F9": True}}
    assert [x["scenario"] for x in p["logs"]] == ["F1", "F9"]
    assert all(x["records"] > 0 and x["anchor"] for x in p["logs"])
    assert set(p["files"]) == {"report.json", "report.html", "raw/F1.evidence.log", "raw/F1.evidence.anchor",
                               "raw/F9.evidence.log", "raw/F9.evidence.anchor"}
    r = pk.verify(d)
    assert r.ok, r.errors


def fr_e3_packet_verify_fails_on_any_changed_file(tmp_path: Path) -> None:
    d = _bench(tmp_path)
    _write(d)
    for rel in ("report.json", "raw/F1.evidence.log", "report.html"):
        f = d / rel
        before = f.read_bytes()
        f.write_bytes(before[:-2] + bytes([before[-2] ^ 1]) + before[-1:])
        r = pk.verify(d)
        assert not r.ok and any(rel in e for e in r.errors), (rel, r.errors)
        f.write_bytes(before)
    (d / "raw" / "F9.evidence.anchor").unlink()
    assert not pk.verify(d).ok
    assert cli_main(["evidence", "verify-packet", str(d)]) == 1


def fr_e3_packet_verify_fails_when_a_target_binary_differs_from_the_build(tmp_path: Path) -> None:
    d = _bench(tmp_path)
    _write(d, target_bin={"arb-guardian": "11" * 32, "arb-arm": "33" * 32})
    r = pk.verify(d)
    assert not r.ok and any("arb-arm" in e for e in r.errors)


def fr_c6_packet_verify_fails_when_a_log_names_another_contract(tmp_path: Path) -> None:
    d = _bench(tmp_path)
    text, anchor = _log([1], contract="ef" * 32)       # a valid chain, for the wrong contract
    (d / "raw" / "F1.evidence.log").write_text(text, encoding="ascii")
    (d / "raw" / "F1.evidence.anchor").write_text(anchor + "\n", encoding="ascii")
    _write(d)
    r = pk.verify(d)
    assert not r.ok and any("contract" in e and "F1" in e for e in r.errors)


def fr_e3_verify_packet_cli_passes_a_good_packet(tmp_path: Path) -> None:
    d = _bench(tmp_path)
    _write(d)
    assert cli_main(["evidence", "verify-packet", str(d)]) == 0


def fr_e3_packet_names_the_policy_the_kernel_reports() -> None:
    """The policy id is read from wherever `secpol -f all` prints it (not its first line)."""
    from arbiter.bench.runner import policy_from

    out = "-----\nSECPOL_BLOB_TYPE_MAP\narb_arm_t = 1\npolicy_id:dc122949  immutable\nallow_attach x y;\n"
    assert policy_from(out) == "policy_id:dc122949  immutable"
    assert policy_from("") == "none"
    assert policy_from("secpol: no policy loaded\n") == "none"
