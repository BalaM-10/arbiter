"""The binary contract table layout, mirrored from the C header.

The single source of truth is ``qnx/common/arb_contract_table.h``. This module
reads its ``#define`` constants and its ``_Static_assert`` offsets at import
time, and checks that the Python ``struct`` layout below agrees with them, so
the host compiler and the guardian's loader cannot drift apart silently.
"""

from __future__ import annotations

import re
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
HEADER = REPO_ROOT / "qnx" / "common" / "arb_contract_table.h"

# Little-endian, no padding: magic, version, table_size, sha256[32],
# joint_min[2], joint_max[2], vmax, silence, rate, burst, threshold, window,
# max_duration, zone_count, zones[4][4], crc32.
LAYOUT = struct.Struct("<IHH32s2i2iIIIIIIII16iI")

# Field order of LAYOUT, used to check offsets against the header.
_FIELD_SIZES: list[tuple[str, int]] = [
    ("magic", 4), ("version", 2), ("table_size", 2), ("source_sha256", 32),
    ("joint_min_urad", 8), ("joint_max_urad", 8), ("vmax_urad_s", 4),
    ("silence_timeout_ms", 4), ("rate_per_s", 4), ("rate_burst", 4),
    ("violation_threshold", 4), ("violation_window_ms", 4), ("max_duration_ms", 4),
    ("zone_count", 4), ("zones", 64), ("crc32", 4),
]


class LayoutMismatchError(RuntimeError):
    """The Python layout disagrees with the C header."""


def _read_header() -> tuple[dict[str, int], dict[str, int], int]:
    text = HEADER.read_text(encoding="utf-8")
    defines: dict[str, int] = {}
    for name, value in re.findall(r"^#define\s+(ARB_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|-?\d+)u?\b", text, re.M):
        defines[name] = int(value, 0)
    offsets = {
        name: int(off)
        for name, off in re.findall(r"offsetof\(arb_contract_table_t,\s*(\w+)\)\s*==\s*(\d+)", text)
    }
    size_match = re.search(r"sizeof\(arb_contract_table_t\)\s*==\s*(\d+)", text)
    if size_match is None:
        raise LayoutMismatchError(f"{HEADER}: no sizeof assertion found")
    return defines, offsets, int(size_match.group(1))


def _field_offsets() -> dict[str, int]:
    offsets: dict[str, int] = {}
    pos = 0
    for name, nbytes in _FIELD_SIZES:
        offsets[name] = pos
        pos += nbytes
    return offsets


def _check_layout(header_offsets: dict[str, int], size: int) -> None:
    ours = _field_offsets()
    if LAYOUT.size != size or sum(n for _, n in _FIELD_SIZES) != size:
        raise LayoutMismatchError(f"Python layout is {LAYOUT.size} bytes, header says {size}")
    for name, off in header_offsets.items():
        if ours.get(name) != off:
            raise LayoutMismatchError(f"field {name}: Python offset {ours.get(name)}, header offset {off}")


DEFINES, HEADER_OFFSETS, TABLE_SIZE = _read_header()
_check_layout(HEADER_OFFSETS, TABLE_SIZE)
OFFSETS = _field_offsets()   # every field; checked above against each offset the header asserts


def const(name: str) -> int:
    """A numeric #define from the C header; fails loudly if it is missing."""
    try:
        return DEFINES[name]
    except KeyError:
        raise LayoutMismatchError(f"{HEADER}: missing #define {name}") from None


JOINTS = const("ARB_JOINTS")
MAX_ZONES = const("ARB_MAX_ZONES")
CRC_OFFSET = OFFSETS["crc32"]


@dataclass(frozen=True)
class Zone:
    x_min_um: int
    y_min_um: int
    x_max_um: int
    y_max_um: int


@dataclass(frozen=True)
class Table:
    """The contract in the table's integer units."""

    source_sha256: bytes
    joint_min_urad: tuple[int, int]
    joint_max_urad: tuple[int, int]
    vmax_urad_s: int
    silence_timeout_ms: int
    rate_per_s: int
    rate_burst: int
    violation_threshold: int
    violation_window_ms: int
    max_duration_ms: int
    zones: tuple[Zone, ...] = field(default_factory=tuple)

    def pack(self) -> bytes:
        """The exact bytes the guardian loads, with the CRC filled in."""
        flat: list[int] = []
        for i in range(MAX_ZONES):
            if i < len(self.zones):
                z = self.zones[i]
                flat += [z.x_min_um, z.y_min_um, z.x_max_um, z.y_max_um]
            else:
                flat += [0, 0, 0, 0]
        body = LAYOUT.pack(
            const("ARB_CONTRACT_MAGIC"), const("ARB_CONTRACT_VERSION"), TABLE_SIZE,
            self.source_sha256, *self.joint_min_urad, *self.joint_max_urad,
            self.vmax_urad_s, self.silence_timeout_ms, self.rate_per_s, self.rate_burst,
            self.violation_threshold, self.violation_window_ms, self.max_duration_ms,
            len(self.zones), *flat, 0,
        )
        crc = zlib.crc32(body[:CRC_OFFSET]) & 0xFFFFFFFF
        return body[:CRC_OFFSET] + struct.pack("<I", crc)

    def dump(self) -> str:
        """Canonical key=value text; the C test prints the same from the binary."""
        packed = self.pack()
        crc = struct.unpack_from("<I", packed, CRC_OFFSET)[0]
        lines = [
            f"magic=0x{const('ARB_CONTRACT_MAGIC'):08x}",
            f"version={const('ARB_CONTRACT_VERSION')}",
            f"table_size={TABLE_SIZE}",
            f"source_sha256={self.source_sha256.hex()}",
            f"joint_min_urad={self.joint_min_urad[0]},{self.joint_min_urad[1]}",
            f"joint_max_urad={self.joint_max_urad[0]},{self.joint_max_urad[1]}",
            f"vmax_urad_s={self.vmax_urad_s}",
            f"silence_timeout_ms={self.silence_timeout_ms}",
            f"rate_per_s={self.rate_per_s}",
            f"rate_burst={self.rate_burst}",
            f"violation_threshold={self.violation_threshold}",
            f"violation_window_ms={self.violation_window_ms}",
            f"max_duration_ms={self.max_duration_ms}",
            f"zone_count={len(self.zones)}",
        ]
        for i in range(MAX_ZONES):
            z = self.zones[i] if i < len(self.zones) else Zone(0, 0, 0, 0)
            lines.append(f"zone{i}={z.x_min_um},{z.y_min_um},{z.x_max_um},{z.y_max_um}")
        lines.append(f"crc32=0x{crc:08x}")
        return "\n".join(lines) + "\n"
