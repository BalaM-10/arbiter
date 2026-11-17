"""Host tests for `arbiter evidence verify` (FR-E2, FR-E4, FR-E5, FR-C6).

The logs here are built by this file's own writer, from docs/EVIDENCE_LOG.md,
not by the code under test. Logs written by arb-logd on QNX are verified in the
fault bench (every scenario) and by the on-target logger tests.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from arbiter import evidence
from arbiter.cli import main as cli_main

ZERO = "0" * 64
CONTRACT = "ab" * 32


def _chain(bodies: list[str]) -> list[str]:
    """Number and hash-chain line bodies ("<tag> <keys...>") per docs/EVIDENCE_LOG.md."""
    out: list[str] = []
    prev = ZERO
    for n, body in enumerate(bodies):
        tag, _, rest = body.partition(" ")
        text = f"{tag} {n} {rest} prev {prev}"
        h = hashlib.sha256(text.encode("ascii")).hexdigest()
        out.append(f"{text} hash {h}")
        prev = h
    return out


def _r(idx: int, seq: int, verdict: int = 1, rule: int = 0) -> str:
    return (f"R idx {idx} t_ns {1000 + idx} seq {seq} target -100000 0 dur 1000 verdict {verdict} "
            f"rule {rule} before 2 after 2 flags 0 fwd -100000 0 1000 latency_cycles {500 + idx}")


def _good(n: int = 5, end: bool = True) -> list[str]:
    bodies = [f"H format 1 contract {CONTRACT} cycles_per_sec 1000000000 start_ns 1"]
    bodies += [_r(i, 100 + i) for i in range(n)]
    if end:
        bodies.append(f"E records {n} lost 0")
    return _chain(bodies)


def _text(lines: list[str]) -> str:
    return "".join(f"{x}\n" for x in lines)


def fr_e2_line_hash_is_sha256_of_the_text_before_hash() -> None:
    # The NIST FIPS 180-2 example: SHA-256("abc").
    assert evidence.line_hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    line = _good(0)[0]
    text, _, h = line.rpartition(" hash ")
    assert evidence.line_hash(text) == h


def fr_e4_verify_passes_on_a_good_log() -> None:
    r = evidence.verify(_text(_good()))
    assert r.ok, r.errors
    assert r.records == 5 and r.lost == 0 and r.terminated


def fr_c6_the_header_carries_the_contract_hash() -> None:
    r = evidence.verify(_text(_good()))
    assert r.contract == CONTRACT
    bad = _chain(["H format 1 cycles_per_sec 1 start_ns 1", _r(0, 1)])
    assert not evidence.verify(_text(bad)).ok


def fr_e4_verify_fails_on_an_edited_record() -> None:
    lines = _good()
    lines[3] = lines[3].replace(" verdict 1 ", " verdict 3 ")     # hash no longer matches
    r = evidence.verify(_text(lines))
    assert not r.ok and "line 3" in r.errors[0]


def fr_e4_verify_fails_on_a_removed_record() -> None:
    lines = _good()
    del lines[2]
    r = evidence.verify(_text(lines))
    assert not r.ok and "line 2" in r.errors[0]


def fr_e4_verify_fails_on_reordered_records() -> None:
    lines = _good()
    lines[2], lines[3] = lines[3], lines[2]
    r = evidence.verify(_text(lines))
    assert not r.ok and "line 2" in r.errors[0]


def fr_e4_verify_fails_on_an_undeclared_gap() -> None:
    # A chain that is consistent in itself, but decision 2 is missing and no G line says so.
    bodies = [f"H format 1 contract {CONTRACT} cycles_per_sec 1 start_ns 1",
              _r(0, 100), _r(1, 101), _r(3, 103)]
    r = evidence.verify(_text(_chain(bodies)))
    assert not r.ok and "line 3" in r.errors[0] and "idx" in r.errors[0]


def fr_e5_a_declared_gap_passes_and_is_counted() -> None:
    bodies = [f"H format 1 contract {CONTRACT} cycles_per_sec 1 start_ns 1", _r(0, 100),
              "G lost 2 from_idx 1 to_idx 2", _r(3, 103), "E records 2 lost 2"]
    r = evidence.verify(_text(_chain(bodies)))
    assert r.ok, r.errors
    assert r.records == 2 and r.lost == 2
    wrong = [f"H format 1 contract {CONTRACT} cycles_per_sec 1 start_ns 1", _r(0, 100),
             "G lost 1 from_idx 1 to_idx 1", _r(3, 103)]                # declares one, two are missing
    assert not evidence.verify(_text(_chain(wrong))).ok


def fr_e4_the_anchor_detects_lines_removed_from_the_end() -> None:
    lines = _good()
    anchor = lines[-1].rpartition(" hash ")[2]
    assert evidence.verify(_text(lines), anchor=anchor).ok
    truncated = evidence.verify(_text(lines[:-2]), anchor=anchor)
    assert not truncated.ok and "anchor" in truncated.errors[0]
    plain = evidence.verify(_text(lines[:-2]))                       # without an anchor: undetectable
    assert plain.ok and not plain.terminated


def fr_e4_verify_rejects_a_header_after_line_0_and_a_bad_line() -> None:
    bodies = [f"H format 1 contract {CONTRACT} cycles_per_sec 1 start_ns 1", _r(0, 1),
              f"H format 1 contract {CONTRACT} cycles_per_sec 1 start_ns 2"]
    assert not evidence.verify(_text(_chain(bodies))).ok
    lines = _good()
    lines.insert(2, "garbage")
    assert not evidence.verify(_text(lines)).ok


def fr_e4_cli_exits_nonzero_on_a_bad_log(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    good = tmp_path / "good.log"
    good.write_text(_text(_good()), encoding="ascii")
    assert cli_main(["evidence", "verify", str(good)]) == 0
    bad = tmp_path / "bad.log"
    lines = _good()
    lines[1] = lines[1].replace(" seq 100 ", " seq 999 ")
    bad.write_text(_text(lines), encoding="ascii")
    assert cli_main(["evidence", "verify", str(bad)]) == 1
    assert "line 1" in capsys.readouterr().out
