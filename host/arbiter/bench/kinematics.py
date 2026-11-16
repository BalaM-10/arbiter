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


def links(q0_urad: float, q1_urad: float) -> tuple[tuple[str, Point, Point], ...]:
    """The two links as named segments: upper arm (base to elbow), forearm (elbow to tip)."""
    elbow, tip = elbow_tip(q0_urad, q1_urad)
    return (("upper arm", (0.0, 0.0), elbow), ("forearm", elbow, tip))


def inside(p: Point, r: Rect) -> bool:
    """Closed rectangle: touching the edge counts as entering."""
    return r.x_min <= p[0] <= r.x_max and r.y_min <= p[1] <= r.y_max


def clearance(p: Point, r: Rect) -> float:
    """Distance in mm from p to the rectangle (0 inside)."""
    dx = max(r.x_min - p[0], 0.0, p[0] - r.x_max)
    dy = max(r.y_min - p[1], 0.0, p[1] - r.y_max)
    return math.hypot(dx, dy)


def _segments_cross(a: Point, b: Point, c: Point, d: Point) -> bool:
    """Do the closed segments a-b and c-d share a point? (orientation test)"""
    def orient(p: Point, q: Point, r: Point) -> float:
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])

    def on(p: Point, q: Point, r: Point) -> bool:   # r on segment p-q, given collinear
        return min(p[0], q[0]) <= r[0] <= max(p[0], q[0]) and min(p[1], q[1]) <= r[1] <= max(p[1], q[1])

    o1, o2, o3, o4 = orient(a, b, c), orient(a, b, d), orient(c, d, a), orient(c, d, b)
    if ((o1 > 0) != (o2 > 0)) and ((o3 > 0) != (o4 > 0)) and 0 not in (o1, o2, o3, o4):
        return True
    return ((o1 == 0 and on(a, b, c)) or (o2 == 0 and on(a, b, d)) or
            (o3 == 0 and on(c, d, a)) or (o4 == 0 and on(c, d, b)))


def _point_segment(p: Point, a: Point, b: Point) -> float:
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = dx * dx + dy * dy
    t = 0.0 if n == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / n))
    return math.hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy))


def segment_clearance(a: Point, b: Point, r: Rect) -> float:
    """Euclidean distance in mm from the segment a-b to the closed rectangle (0 if they touch).

    A different method from the guardian's on purpose: the segment touches the
    rectangle if an end is inside or it crosses one of the four edges; otherwise the
    distance is the smallest of the ends' distances to the rectangle and the
    corners' distances to the segment.
    """
    if inside(a, r) or inside(b, r):
        return 0.0
    corners = [(r.x_min, r.y_min), (r.x_max, r.y_min), (r.x_max, r.y_max), (r.x_min, r.y_max)]
    edges = list(zip(corners, corners[1:] + corners[:1], strict=True))
    if any(_segments_cross(a, b, c, d) for c, d in edges):
        return 0.0
    return min(clearance(a, r), clearance(b, r), *(_point_segment(c, a, b) for c in corners))
