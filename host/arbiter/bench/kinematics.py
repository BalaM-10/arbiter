"""Independent forward kinematics of the 2-joint planar arm (FR-A2), in Python.

Written from the geometry in ``arb_arm_model.h`` and nothing else: it shares no
code with libcontract, so a bug in the guardian's keep-out check cannot hide
itself here.
"""

from __future__ import annotations

import math

from .spec import LINK1_MM, LINK2_MM, Rect

Point = tuple[float, float]


def elbow_tip(q0_urad: float, q1_urad: float) -> tuple[Point, Point]:
    """Elbow and tip in mm, base at the origin."""
    a = q0_urad * 1e-6
    b = a + q1_urad * 1e-6
    elbow = (LINK1_MM * math.cos(a), LINK1_MM * math.sin(a))
    tip = (elbow[0] + LINK2_MM * math.cos(b), elbow[1] + LINK2_MM * math.sin(b))
    return elbow, tip


def inside(p: Point, r: Rect) -> bool:
    """Closed rectangle: touching the edge counts as entering."""
    return r.x_min <= p[0] <= r.x_max and r.y_min <= p[1] <= r.y_max


def clearance(p: Point, r: Rect) -> float:
    """Distance in mm from p to the rectangle (0 inside)."""
    dx = max(r.x_min - p[0], 0.0, p[0] - r.x_max)
    dy = max(r.y_min - p[1], 0.0, p[1] - r.y_max)
    return math.hypot(dx, dy)
