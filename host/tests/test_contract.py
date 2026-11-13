"""Tests for the contract compiler. Each test is named after its requirement."""

from __future__ import annotations

import hashlib
import math
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import pytest

from arbiter import table as tbl
from arbiter.cli import main as cli_main
from arbiter.contract import ContractError, compile_bytes, compile_file
from arbiter.corpus import write_corpus

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT / "contracts" / "default.yaml"


def _contract(**overrides: str) -> bytes:
    """The default contract text with whole top-level sections replaced."""
    sections = {
        "contract_version": "contract_version: 1",
        "joints": ("joints:\n  shoulder: {min_deg: -150, max_deg: 150}\n"
                   "  elbow: {min_deg: -135, max_deg: 135}"),
        "max_joint_speed_deg_per_s": "max_joint_speed_deg_per_s: 57.29578",
        "max_move_duration_ms": "max_move_duration_ms: 10000",
        "ai_silence_timeout_ms": "ai_silence_timeout_ms: 250",
        "command_rate": "command_rate: {per_second: 50, burst: 10}",
        "violations": "violations: {threshold: 3, window_ms: 1000}",
        "keep_out_zones": ("keep_out_zones:\n"
                           "  - {name: operator_hand_area, x_mm: [200, 400], y_mm: [150, 350]}"),
    }
    sections.update(overrides)
    return ("\n".join(v for v in sections.values() if v) + "\n").encode()


def _problems(source: bytes) -> list[str]:
    with pytest.raises(ContractError) as info:
        compile_bytes(source)
    return info.value.problems


# ------------------------------------------------------------------- FR-C1

def fr_c1_default_contract_compiles_with_every_field() -> None:
    t = compile_file(DEFAULT)
    # 150 deg = 2617993.88 urad, rounded inward; 135 deg = 2356194.49 urad.
    assert t.joint_min_urad == (-2617993, -2356194)
    assert t.joint_max_urad == (2617993, 2356194)
    assert t.vmax_urad_s == 1000000           # 57.29578 deg/s = 1.0000000085 rad/s, rounded down
    assert t.max_duration_ms == 10000
    assert t.silence_timeout_ms == 250
    assert (t.rate_per_s, t.rate_burst) == (50, 10)
    assert (t.violation_threshold, t.violation_window_ms) == (3, 1000)
    assert [(z.x_min_um, z.y_min_um, z.x_max_um, z.y_max_um) for z in t.zones] == [
        (200000, 150000, 400000, 350000)]


def fr_c1_no_zones_is_a_valid_contract() -> None:
    t = compile_bytes(_contract(keep_out_zones="keep_out_zones: []"))
    assert t.zones == ()


# ------------------------------------------------------------------- FR-C2

def fr_c2_missing_value_rejected_by_name() -> None:
    assert "ai_silence_timeout_ms: missing" in _problems(_contract(ai_silence_timeout_ms=""))
    probs = _problems(_contract(
        joints="joints:\n  shoulder: {min_deg: -150}\n  elbow: {min_deg: -1, max_deg: 1}"))
    assert "joints.shoulder.max_deg: missing" in probs


def fr_c2_unknown_key_rejected() -> None:
    probs = _problems(_contract(command_rate="command_rate: {per_second: 50, burst: 10, brust: 3}"))
    assert "command_rate.brust: unknown key" in probs
    assert "max_speed: unknown key" in _problems(_contract() + b"max_speed: 3\n")


def fr_c2_duplicate_key_rejected() -> None:
    probs = _problems(_contract() + b"ai_silence_timeout_ms: 9999\n")
    assert len(probs) == 1 and "duplicate key 'ai_silence_timeout_ms'" in probs[0]


def fr_c2_wrong_types_rejected() -> None:
    assert any("must be a number" in p for p in _problems(_contract(
        max_joint_speed_deg_per_s="max_joint_speed_deg_per_s: fast")))
    assert any("must be a whole number" in p for p in _problems(_contract(
        ai_silence_timeout_ms="ai_silence_timeout_ms: 250.5")))
    # YAML booleans are not numbers, even though Python's bool is an int.
    assert any("must be a whole number, got True" in p for p in _problems(_contract(
        ai_silence_timeout_ms="ai_silence_timeout_ms: true")))
    assert "joints: must be a mapping" in _problems(_contract(joints="joints: [1, 2]"))
    assert "keep_out_zones: must be a list (use [] for none)" in _problems(_contract(
        keep_out_zones="keep_out_zones: hands"))
    assert ": must be a mapping" in _problems(b"- just\n- a list\n")


def fr_c2_non_finite_numbers_rejected() -> None:
    assert any("must be finite" in p for p in _problems(_contract(
        max_joint_speed_deg_per_s="max_joint_speed_deg_per_s: .nan")))
    assert any("must be finite" in p for p in _problems(_contract(
        joints="joints:\n  shoulder: {min_deg: -.inf, max_deg: 150}\n  elbow: {min_deg: -1, max_deg: 1}")))


def fr_c2_out_of_range_values_rejected() -> None:
    assert any(p.startswith("joints.shoulder.max_deg: must be between") for p in _problems(_contract(
        joints="joints:\n  shoulder: {min_deg: -150, max_deg: 190}\n  elbow: {min_deg: -1, max_deg: 1}")))
    assert any(p.startswith("command_rate.per_second: must be between") for p in _problems(_contract(
        command_rate="command_rate: {per_second: 0, burst: 10}")))
    assert any(p.startswith("violations.threshold: must be between") for p in _problems(_contract(
        violations="violations: {threshold: 17, window_ms: 1000}")))
    assert any(p.startswith("max_move_duration_ms: must be between") for p in _problems(_contract(
        max_move_duration_ms="max_move_duration_ms: 0")))
    assert any(p.startswith("keep_out_zones[0].x_mm[1]: must be between") for p in _problems(_contract(
        keep_out_zones="keep_out_zones:\n  - {name: p, x_mm: [0, 2500], y_mm: [0, 1]}")))


def fr_c2_contradictions_rejected() -> None:
    assert "joints.elbow: min_deg (10) must be less than max_deg (10)" in _problems(_contract(
        joints="joints:\n  shoulder: {min_deg: -150, max_deg: 150}\n  elbow: {min_deg: 10, max_deg: 10}"))
    assert "keep_out_zones[0].y_mm: min (350) must be less than max (150)" in _problems(_contract(
        keep_out_zones="keep_out_zones:\n  - {name: p, x_mm: [200, 400], y_mm: [350, 150]}"))
    probs = _problems(_contract(max_move_duration_ms="max_move_duration_ms: 5000"))
    assert any("shorter than a full-range move of the shoulder joint" in p for p in probs)
    assert any("duplicate zone name 'p'" in p for p in _problems(_contract(
        keep_out_zones="keep_out_zones:\n  - {name: p, x_mm: [0, 1], y_mm: [0, 1]}\n"
                       "  - {name: p, x_mm: [2, 3], y_mm: [2, 3]}")))
    five = "".join(f"  - {{name: z{i}, x_mm: [{i}, {i + 1}], y_mm: [0, 1]}}\n" for i in range(5))
    assert "keep_out_zones: at most 4 zones, got 5" in _problems(
        _contract(keep_out_zones="keep_out_zones:\n" + five))
    assert "contract_version: must be 1, got 2" in _problems(
        _contract(contract_version="contract_version: 2"))


def fr_c2_every_problem_reported_at_once() -> None:
    probs = _problems(_contract(ai_silence_timeout_ms="",
                                command_rate="command_rate: {per_second: 0, burst: 0}"))
    assert len(probs) >= 3


def fr_c2_not_utf8_and_not_yaml_rejected() -> None:
    assert any("not UTF-8" in p for p in _problems(b"\xff\xfe\x00"))
    assert any(p.startswith("YAML:") for p in _problems(b"joints: [unclosed\n"))


def fr_c2_rounding_is_toward_safety() -> None:
    t = compile_bytes(_contract(
        joints="joints:\n  shoulder: {min_deg: -100.00001, max_deg: 100.00001}\n"
               "  elbow: {min_deg: -90.5, max_deg: 90.5}",
        max_joint_speed_deg_per_s="max_joint_speed_deg_per_s: 30.00001",
        keep_out_zones="keep_out_zones:\n  - {name: p, x_mm: [200.0004, 399.9996], y_mm: [150.5, 349.5]}"))
    limits_deg = ((-100.00001, -90.5), (100.00001, 90.5))
    for lo_u, hi_u, lo_d, hi_d in zip(t.joint_min_urad, t.joint_max_urad, *limits_deg, strict=True):
        assert lo_u >= lo_d * math.pi / 180 * 1e6          # inward
        assert hi_u <= hi_d * math.pi / 180 * 1e6
    assert t.vmax_urad_s <= 30.00001 * math.pi / 180 * 1e6  # never faster
    z = t.zones[0]
    assert (z.x_min_um, z.x_max_um, z.y_min_um, z.y_max_um) == (200000, 400000, 150500, 349500)  # outward


def fr_c2_cli_reports_problems_and_exits_2(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    bad = tmp_path / "bad.yaml"
    bad.write_bytes(_contract(ai_silence_timeout_ms="ai_silence_timeout_ms: 5"))
    assert cli_main(["contract", "check", str(bad)]) == 2
    err = capsys.readouterr().err
    assert "contract invalid" in err and "ai_silence_timeout_ms: must be between 10 and 60000, got 5" in err


# ------------------------------------------------------------------- FR-C3

def fr_c3_layout_matches_the_c_header() -> None:
    assert tbl.LAYOUT.size == tbl.TABLE_SIZE == 156
    assert tbl.OFFSETS["crc32"] == 152 and tbl.OFFSETS["zones"] == 88


def fr_c3_table_has_version_crc_and_source_hash(tmp_path: Path) -> None:
    out = tmp_path / "c.bin"
    assert cli_main(["contract", "compile", str(DEFAULT), "-o", str(out)]) == 0
    data = out.read_bytes()
    magic, version, size = struct.unpack_from("<IHH", data, 0)
    assert (magic, version, size) == (tbl.const("ARB_CONTRACT_MAGIC"), 1, 156) and len(data) == 156
    assert data[8:40] == hashlib.sha256(DEFAULT.read_bytes()).digest()
    assert struct.unpack_from("<I", data, 152)[0] == zlib.crc32(data[:152]) & 0xFFFFFFFF


def fr_c3_source_hash_changes_with_any_byte_of_the_source() -> None:
    a = compile_bytes(_contract())
    b = compile_bytes(_contract() + b"# a comment changes the hash, not the limits\n")
    assert a.source_sha256 != b.source_sha256
    assert a.joint_max_urad == b.joint_max_urad


def fr_c3_compiling_is_deterministic() -> None:
    assert compile_file(DEFAULT).pack() == compile_file(DEFAULT).pack()


# ---------------------------------------------- FR-C3 / FR-C4: cross-language

@pytest.fixture(scope="session")
def c_loader(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """qnx/tests/test_contract_file.c built for the host with the real libcontract."""
    cc = shutil.which("gcc")
    if cc is None:
        pytest.skip("gcc not installed")
    exe = tmp_path_factory.mktemp("cloader") / "test_contract_file"
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         f"-I{ROOT / 'qnx/common'}", f"-I{ROOT / 'qnx/libcontract'}",
         str(ROOT / "qnx/tests/test_contract_file.c"), str(ROOT / "qnx/libcontract/arb_contract.c"),
         "-lm", "-o", str(exe)],
        check=True)
    return exe


def fr_c3_c_loader_reads_exactly_what_the_compiler_wrote(c_loader: Path, tmp_path: Path) -> None:
    manifest = write_corpus(compile_file(DEFAULT), tmp_path / "corpus")
    run = subprocess.run([str(c_loader), str(manifest)], capture_output=True, text=True, check=False)
    assert run.returncode == 0, run.stdout
    assert "PASS fr_c3_compiled_table_matches_source_dump contract.bin" in run.stdout


def fr_c4_c_loader_rejects_every_corrupted_variant(c_loader: Path, tmp_path: Path) -> None:
    manifest = write_corpus(compile_file(DEFAULT), tmp_path / "corpus")
    run = subprocess.run([str(c_loader), str(manifest)], capture_output=True, text=True, check=False)
    rejects = [ln for ln in run.stdout.splitlines() if "fr_c4_corrupted_compiled_table_rejected" in ln]
    assert len(rejects) == 10 and all(ln.startswith("PASS") for ln in rejects), run.stdout


def fr_c4_c_loader_test_detects_a_wrong_expectation(c_loader: Path, tmp_path: Path) -> None:
    """The cross-language test itself can fail: a doctored dump must be reported."""
    manifest = write_corpus(compile_file(DEFAULT), tmp_path / "corpus")
    dump = tmp_path / "corpus" / "contract.dump"
    dump.write_text(dump.read_text().replace("rate_burst=10", "rate_burst=11"))
    run = subprocess.run([str(c_loader), str(manifest)], capture_output=True, text=True, check=False)
    assert run.returncode != 0 and "FAIL fr_c3_compiled_table_matches_source_dump" in run.stdout
