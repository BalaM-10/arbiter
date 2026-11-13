"""Cross-language test corpus for the contract table (FR-C3, FR-C4).

From one contract, write a directory with the compiled table, its canonical
dump, and deliberately broken variants, plus a manifest saying what the C
loader must do with each. ``qnx/tests/test_contract_file.c`` runs the manifest
on the host and on the QNX target, so the Python compiler and the guardian's
loader are checked against each other byte for byte.

Manifest lines:
    ok <table.bin> <expected.dump>
    reject <table.bin> <ARB_LOAD_* code> <what was broken>
"""

from __future__ import annotations

import re
import struct
import zlib
from pathlib import Path

from . import table as tbl
from .table import Table

LOADER_HEADER = tbl.REPO_ROOT / "qnx" / "libcontract" / "arb_contract.h"


def load_codes() -> dict[str, int]:
    """ARB_LOAD_* codes from the C header, so the manifest never hard-codes them."""
    text = LOADER_HEADER.read_text(encoding="utf-8")
    codes = {name: int(v) for name, v in re.findall(r"\b(ARB_LOAD_[A-Z_]+)\s*=\s*(\d+)", text)}
    if "ARB_LOAD_OK" not in codes:
        raise tbl.LayoutMismatchError(f"{LOADER_HEADER}: ARB_LOAD_* codes not found")
    return codes


def _reseal(data: bytearray) -> bytes:
    crc = zlib.crc32(bytes(data[: tbl.CRC_OFFSET])) & 0xFFFFFFFF
    struct.pack_into("<I", data, tbl.CRC_OFFSET, crc)
    return bytes(data)


def _set_u32(data: bytes, field: str, value: int) -> bytes:
    buf = bytearray(data)
    struct.pack_into("<I", buf, tbl.OFFSETS[field], value)
    return _reseal(buf)


def _set_u16(data: bytes, field: str, value: int) -> bytes:
    buf = bytearray(data)
    struct.pack_into("<H", buf, tbl.OFFSETS[field], value)
    return _reseal(buf)


def variants(good: bytes, t: Table) -> list[tuple[str, bytes, str, str]]:
    """(file name, bytes, expected code name, description) for each broken table."""
    flipped = bytearray(good)
    flipped[tbl.OFFSETS["source_sha256"]] ^= 0x01            # any byte change -> CRC mismatch
    return [
        ("truncated.bin", good[:-1], "ARB_LOAD_E_SIZE", "one byte short"),
        ("extended.bin", good + b"\x00", "ARB_LOAD_E_SIZE", "one byte long"),
        ("empty.bin", b"", "ARB_LOAD_E_SIZE", "zero length"),
        ("magic.bin", _set_u32(good, "magic", 0x43425242), "ARB_LOAD_E_MAGIC", "wrong magic, CRC resealed"),
        ("version.bin", _set_u16(good, "version", tbl.const("ARB_CONTRACT_VERSION") + 1),
         "ARB_LOAD_E_VERSION", "future version, CRC resealed"),
        ("tablesize.bin", _set_u16(good, "table_size", tbl.TABLE_SIZE - 4),
         "ARB_LOAD_E_TABLESIZE", "wrong size field, CRC resealed"),
        ("crc.bin", bytes(flipped), "ARB_LOAD_E_CRC", "one source-hash byte flipped, CRC not resealed"),
        ("vmax0.bin", _set_u32(good, "vmax_urad_s", 0), "ARB_LOAD_E_RANGE", "zero speed limit, CRC resealed"),
        ("shortmax.bin", _set_u32(good, "max_duration_ms", 1), "ARB_LOAD_E_RANGE",
         "max duration shorter than a full-range move, CRC resealed"),
        ("zones5.bin", _set_u32(good, "zone_count", tbl.MAX_ZONES + 1), "ARB_LOAD_E_RANGE",
         "too many zones, CRC resealed"),
    ]


def write_corpus(t: Table, out_dir: Path, stem: str = "contract") -> Path:
    """Write the corpus and its manifest; returns the manifest path."""
    codes = load_codes()
    out_dir.mkdir(parents=True, exist_ok=True)
    good = t.pack()
    (out_dir / f"{stem}.bin").write_bytes(good)
    (out_dir / f"{stem}.dump").write_text(t.dump(), encoding="utf-8")
    lines = [f"ok {stem}.bin {stem}.dump"]
    for name, data, code, what in variants(good, t):
        (out_dir / name).write_bytes(data)
        lines.append(f"reject {name} {codes[code]} {what}")
    manifest = out_dir / "manifest.txt"
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return manifest
