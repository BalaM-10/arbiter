"""Safety-contract compiler (FR-C1, FR-C2, FR-C3, FR-C6).

Reads one YAML contract, validates every value, converts to the table's integer
units and emits the fixed-layout binary table the guardian loads.

Validation is strict: unknown keys, duplicate keys, wrong types (including
booleans where numbers are expected), missing values, out-of-range values and
contradictions are all rejected with a message naming the exact key.

Rounding always moves toward safety: joint limits inward, the speed limit down,
keep-out zones outward. A compiled table is never more permissive than its source.
"""

from __future__ import annotations

import hashlib
import math
from collections.abc import Set as AbstractSet
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from . import table as tbl
from .table import Table, Zone

CONTRACT_VERSION = 1
JOINT_NAMES = ("shoulder", "elbow")
URAD_PER_DEG = math.pi / 180.0 * 1e6


# A keep-out zone must keep this far from the arm's base (the shoulder joint).
BASE_CLEARANCE_MM = 20.0


class ContractError(ValueError):
    """The contract is invalid. ``str(err)`` lists every problem, one per line."""

    def __init__(self, problems: list[str]) -> None:
        self.problems = problems
        super().__init__("\n".join(problems))


# --------------------------------------------------------------------- YAML

class _StrictLoader(yaml.SafeLoader):
    """SafeLoader that rejects duplicate mapping keys (PyYAML accepts them silently)."""


def _construct_mapping(loader: _StrictLoader, node: yaml.MappingNode, deep: bool = False) -> dict[Any, Any]:
    seen: set[Any] = set()
    for key_node, _ in node.value:
        key = key_node.value if isinstance(key_node, yaml.ScalarNode) else repr(key_node)
        if key in seen:
            raise yaml.constructor.ConstructorError(
                None, None, f"duplicate key {key!r}", key_node.start_mark)
        seen.add(key)
    return loader.construct_mapping(node, deep=deep)


_StrictLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_mapping)


def parse_yaml(text: str) -> Any:
    try:
        return yaml.load(text, Loader=_StrictLoader)
    except yaml.YAMLError as exc:
        raise ContractError([f"YAML: {exc}"]) from exc


# --------------------------------------------------------------- validation

@dataclass
class _Checker:
    problems: list[str]

    def mapping(self, value: Any, path: str, required: AbstractSet[str],
                optional: AbstractSet[str] = frozenset()) -> dict[str, Any]:
        if not isinstance(value, dict):
            self.problems.append(f"{path}: must be a mapping")
            return {}
        for key in sorted(set(value) - required - optional, key=str):
            self.problems.append(f"{path}.{key}: unknown key" if path else f"{key}: unknown key")
        for key in sorted(required - set(value)):
            self.problems.append(f"{path}.{key}: missing" if path else f"{key}: missing")
        return value

    def number(self, value: Any, path: str, lo: float, hi: float) -> float | None:
        if value is None:
            return None  # already reported as missing
        if isinstance(value, bool) or not isinstance(value, int | float):
            self.problems.append(f"{path}: must be a number, got {value!r}")
            return None
        if not math.isfinite(value):
            self.problems.append(f"{path}: must be finite, got {value!r}")
            return None
        if not lo <= value <= hi:
            self.problems.append(f"{path}: must be between {lo:g} and {hi:g}, got {value:g}")
            return None
        return float(value)

    def integer(self, value: Any, path: str, lo: int, hi: int) -> int | None:
        if value is None:
            return None
        if isinstance(value, bool) or not isinstance(value, int):
            self.problems.append(f"{path}: must be a whole number, got {value!r}")
            return None
        if not lo <= value <= hi:
            self.problems.append(f"{path}: must be between {lo} and {hi}, got {value}")
            return None
        return int(value)


def _deg_inward(deg: float, is_min: bool) -> int:
    """Degrees -> microradians, rounded inward (toward a narrower range)."""
    urad = deg * URAD_PER_DEG
    return math.ceil(urad) if is_min else math.floor(urad)


def _mm_outward(mm: float, is_min: bool) -> int:
    """Millimetres -> micrometres, rounded outward (toward a larger zone)."""
    um = mm * 1000.0
    return math.floor(um) if is_min else math.ceil(um)


def validate(doc: Any, source_sha256: bytes) -> Table:
    """Validate a parsed contract and convert it to table units. Raises ContractError."""
    c = _Checker([])
    top = c.mapping(doc, "", {
        "contract_version", "joints", "max_joint_speed_deg_per_s", "max_move_duration_ms",
        "ai_silence_timeout_ms", "command_rate", "violations", "keep_out_zones"})

    if "contract_version" in top and top["contract_version"] != CONTRACT_VERSION:
        c.problems.append(f"contract_version: must be {CONTRACT_VERSION}, got {top['contract_version']!r}")

    limit_deg = tbl.const("ARB_ANGLE_LIMIT_URAD") / URAD_PER_DEG
    jmin: list[int] = []
    jmax: list[int] = []
    joints = c.mapping(top.get("joints"), "joints", set(JOINT_NAMES)) if "joints" in top else {}
    for name in JOINT_NAMES:
        j = c.mapping(joints.get(name), f"joints.{name}", {"min_deg", "max_deg"}) if name in joints else {}
        lo = c.number(j.get("min_deg"), f"joints.{name}.min_deg", -limit_deg, limit_deg)
        hi = c.number(j.get("max_deg"), f"joints.{name}.max_deg", -limit_deg, limit_deg)
        if lo is not None and hi is not None:
            if lo >= hi:
                c.problems.append(f"joints.{name}: min_deg ({lo:g}) must be less than max_deg ({hi:g})")
            else:
                jmin.append(_deg_inward(lo, True))
                jmax.append(_deg_inward(hi, False))

    speed = c.number(top.get("max_joint_speed_deg_per_s"), "max_joint_speed_deg_per_s",
                     tbl.const("ARB_VMAX_MIN_URAD_S") / URAD_PER_DEG,
                     tbl.const("ARB_VMAX_MAX_URAD_S") / URAD_PER_DEG)
    vmax = math.floor(speed * URAD_PER_DEG) if speed is not None else None   # round down: never faster
    if vmax is not None and vmax < tbl.const("ARB_VMAX_MIN_URAD_S"):
        c.problems.append(f"max_joint_speed_deg_per_s: {speed:g} rounds below the minimum allowed speed")
        vmax = None

    max_dur = c.integer(top.get("max_move_duration_ms"), "max_move_duration_ms",
                        tbl.const("ARB_DURATION_MIN_MS"), tbl.const("ARB_DURATION_CEIL_MS"))
    silence = c.integer(top.get("ai_silence_timeout_ms"), "ai_silence_timeout_ms",
                        tbl.const("ARB_SILENCE_MIN_MS"), tbl.const("ARB_SILENCE_MAX_MS"))

    rate = c.mapping(top.get("command_rate"), "command_rate", {"per_second", "burst"}) \
        if "command_rate" in top else {}
    per_s = c.integer(rate.get("per_second"), "command_rate.per_second",
                      tbl.const("ARB_RATE_MIN_PER_S"), tbl.const("ARB_RATE_MAX_PER_S"))
    burst = c.integer(rate.get("burst"), "command_rate.burst",
                      tbl.const("ARB_BURST_MIN"), tbl.const("ARB_BURST_MAX"))

    viol = c.mapping(top.get("violations"), "violations", {"threshold", "window_ms"}) \
        if "violations" in top else {}
    threshold = c.integer(viol.get("threshold"), "violations.threshold",
                          1, tbl.const("ARB_MAX_VIOLATION_THRESHOLD"))
    window = c.integer(viol.get("window_ms"), "violations.window_ms",
                       tbl.const("ARB_WINDOW_MIN_MS"), tbl.const("ARB_WINDOW_MAX_MS"))

    zones: list[Zone] = []
    raw_zones = top.get("keep_out_zones", None)
    ws_mm = tbl.const("ARB_WORKSPACE_LIMIT_UM") / 1000.0
    if "keep_out_zones" in top:
        if not isinstance(raw_zones, list):
            c.problems.append("keep_out_zones: must be a list (use [] for none)")
        elif len(raw_zones) > tbl.MAX_ZONES:
            c.problems.append(f"keep_out_zones: at most {tbl.MAX_ZONES} zones, got {len(raw_zones)}")
        else:
            names: set[str] = set()
            for i, rz in enumerate(raw_zones):
                p = f"keep_out_zones[{i}]"
                z = c.mapping(rz, p, {"name", "x_mm", "y_mm"})
                zone_name = z.get("name")
                if "name" in z:
                    if not isinstance(zone_name, str) or not zone_name:
                        c.problems.append(f"{p}.name: must be a non-empty string")
                    elif zone_name in names:
                        c.problems.append(f"{p}.name: duplicate zone name {zone_name!r}")
                    else:
                        names.add(zone_name)
                bounds: list[int] = []
                ext: list[tuple[float, float]] = []     # as written, mm: x then y
                for axis in ("x_mm", "y_mm"):
                    v = z.get(axis)
                    if axis not in z:
                        continue
                    if not isinstance(v, list) or len(v) != 2:
                        c.problems.append(f"{p}.{axis}: must be [min, max]")
                        continue
                    lo = c.number(v[0], f"{p}.{axis}[0]", -ws_mm, ws_mm)
                    hi = c.number(v[1], f"{p}.{axis}[1]", -ws_mm, ws_mm)
                    if lo is None or hi is None:
                        continue
                    if lo >= hi:
                        c.problems.append(f"{p}.{axis}: min ({lo:g}) must be less than max ({hi:g})")
                        continue
                    bounds += [_mm_outward(lo, True), _mm_outward(hi, False)]
                    ext.append((lo, hi))
                lim = tbl.const("ARB_WORKSPACE_LIMIT_UM")
                if len(bounds) == 4 and any(abs(b) > lim for b in bounds):
                    c.problems.append(f"{p}: rounds outside the workspace limit of {ws_mm:g} mm")
                elif len(bounds) == 4:
                    # The upper arm always touches the base, so a zone there could
                    # never be left (owner decision 2026-11-16).
                    near = math.hypot(*(max(lo - 0.0, 0.0, 0.0 - hi) for lo, hi in ext))
                    if near < BASE_CLEARANCE_MM:
                        c.problems.append(f"{p}: comes within {BASE_CLEARANCE_MM:g} mm of the arm's base "
                                          f"({near:.1f} mm)")
                    else:
                        zones.append(Zone(bounds[0], bounds[2], bounds[1], bounds[3]))

    # Cross-field rule (mirrors arb_contract_load): a full-range move at vmax
    # must fit within max_move_duration_ms.
    if len(jmin) == tbl.JOINTS and vmax is not None and max_dur is not None:
        for name, lo_u, hi_u in zip(JOINT_NAMES, jmin, jmax, strict=True):
            full_ms = math.ceil((hi_u - lo_u) * 1000 / vmax)
            if full_ms > max_dur:
                c.problems.append(
                    f"max_move_duration_ms: {max_dur} ms is shorter than a full-range move of the "
                    f"{name} joint at the speed limit ({full_ms} ms)")

    if c.problems:
        raise ContractError(c.problems)
    assert vmax is not None and silence is not None and per_s is not None and burst is not None
    assert threshold is not None and window is not None and max_dur is not None
    return Table(
        source_sha256=source_sha256,
        joint_min_urad=(jmin[0], jmin[1]),
        joint_max_urad=(jmax[0], jmax[1]),
        vmax_urad_s=vmax,
        silence_timeout_ms=silence,
        rate_per_s=per_s,
        rate_burst=burst,
        violation_threshold=threshold,
        violation_window_ms=window,
        max_duration_ms=max_dur,
        zones=tuple(zones),
    )


def compile_bytes(source: bytes) -> Table:
    """Validate a contract given as the YAML file's bytes."""
    try:
        text = source.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ContractError([f"file is not UTF-8: {exc}"]) from exc
    return validate(parse_yaml(text), hashlib.sha256(source).digest())


def compile_file(path: Path) -> Table:
    return compile_bytes(path.read_bytes())
