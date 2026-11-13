"""What the bench judges against, read independently of the guardian.

- Limits come straight from the contract YAML as written (degrees, mm), not from
  the compiled table, so the compiler's rounding and the guardian's margins are
  not part of the judgment. The contract is still validated by the compiler.
- Arm geometry and the control period come from ``qnx/common/arb_arm_model.h``.
- Verdict, rule and state codes come from ``qnx/libcontract/arb_contract.h`` and
  ``qnx/arm/arm_motion.h``.
Header values are parsed at import time, so the bench cannot drift from the C code.
"""

from __future__ import annotations

import hashlib
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from ..contract import compile_file

REPO_ROOT = Path(__file__).resolve().parents[3]
ARM_MODEL_H = REPO_ROOT / "qnx" / "common" / "arb_arm_model.h"
CONTRACT_H = REPO_ROOT / "qnx" / "libcontract" / "arb_contract.h"
ARM_MOTION_H = REPO_ROOT / "qnx" / "arm" / "arm_motion.h"


def _defines(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    return {n: int(v) for n, v in re.findall(r"^#define\s+(ARB_\w+)\s+(-?\d+)L?\b", text, re.M)}


def _enum(path: Path, prefix: str) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    return {n: int(v) for n, v in re.findall(rf"\b{prefix}(\w+)\s*=\s*(\d+)", text)}


_ARM = _defines(ARM_MODEL_H)
LINK1_MM = _ARM["ARB_LINK1_UM"] / 1000.0
LINK2_MM = _ARM["ARB_LINK2_UM"] / 1000.0
PERIOD_NS = _ARM["ARB_ARM_PERIOD_NS"]

VERDICT = _enum(CONTRACT_H, "ARB_VERDICT_")
RULE = _enum(CONTRACT_H, "ARB_RULE_")
STATE = _enum(CONTRACT_H, "ARB_STATE_")
ARM_STATE = _enum(ARM_MOTION_H, "ARM_STATE_")
VERDICT_NAME = {v: k for k, v in VERDICT.items()}
RULE_NAME = {v: k for k, v in RULE.items()}
STATE_NAME = {v: k for k, v in STATE.items()}

URAD_PER_DEG = math.pi / 180.0 * 1e6


@dataclass(frozen=True)
class Rect:
    name: str
    x_min: float
    x_max: float
    y_min: float
    y_max: float


@dataclass(frozen=True)
class Spec:
    """The contract as written, in µrad and mm (floats: no rounding toward anything)."""

    source: Path
    sha256: str
    joint_min_urad: tuple[float, float]
    joint_max_urad: tuple[float, float]
    vmax_urad_s: float
    silence_timeout_ms: int
    zones: tuple[Rect, ...]


def load_spec(path: Path) -> Spec:
    compile_file(path)                      # reject an invalid contract before judging anything
    raw = path.read_bytes()
    doc: dict[str, Any] = yaml.safe_load(raw)
    joints = [doc["joints"]["shoulder"], doc["joints"]["elbow"]]
    zones = tuple(
        Rect(str(z["name"]), float(z["x_mm"][0]), float(z["x_mm"][1]),
             float(z["y_mm"][0]), float(z["y_mm"][1]))
        for z in doc["keep_out_zones"]
    )
    return Spec(
        source=path,
        sha256=hashlib.sha256(raw).hexdigest(),
        joint_min_urad=(joints[0]["min_deg"] * URAD_PER_DEG, joints[1]["min_deg"] * URAD_PER_DEG),
        joint_max_urad=(joints[0]["max_deg"] * URAD_PER_DEG, joints[1]["max_deg"] * URAD_PER_DEG),
        vmax_urad_s=float(doc["max_joint_speed_deg_per_s"]) * URAD_PER_DEG,
        silence_timeout_ms=int(doc["ai_silence_timeout_ms"]),
        zones=zones,
    )
