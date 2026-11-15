"""Checks shared by the scenarios. Each returns a Check with the evidence that decided it.

Telemetry checks are the independent truth: they use only the arm's samples
(position and the time it was computed for), the contract as written, and the
bench's own kinematics.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from itertools import pairwise
from typing import Any

from . import kinematics as kin
from .records import Run, Verdict
from .script import Command, Script
from .spec import ARM_STATE, PERIOD_NS, RULE, RULE_NAME, STATE, STATE_NAME, VERDICT, VERDICT_NAME, Spec

# Integer rounding of two positions (each llround'ed by the arm) between samples.
POS_ROUNDING_URAD = 2
REACHED_TOL_URAD = 10
KEEPOUT_SUBSTEPS = 8        # joint-linear points checked between consecutive samples


@dataclass
class Check:
    name: str
    passed: bool
    detail: str
    data: dict[str, Any] = field(default_factory=dict)


def _vname(v: Verdict) -> str:
    return f"{VERDICT_NAME.get(v.verdict, v.verdict)}/{RULE_NAME.get(v.rule, v.rule)}"


# ---------------------------------------------------------------- run health

def run_complete(run: Run, script: Script) -> Check:
    problems = []
    if run.exit_code != 0:
        problems.append(f"arb-inject exit {run.exit_code}")
    if not run.done:
        problems.append("no DONE line")
    if len(run.verdicts) != len(script.commands):
        problems.append(f"{len(run.verdicts)} verdicts for {len(script.commands)} commands")
    if any(v.rc != 0 for v in run.verdicts):
        problems.append("a devctl() call failed")
    return Check("run_complete", not problems, "; ".join(problems) or
                 f"{len(run.verdicts)} commands sent, {len(run.samples)} samples recorded")


def trusted_alive(run: Run) -> Check:
    ok = run.alive == {"watchdog": 1, "arm": 1, "guardian": 1}
    return Check("trusted_processes_alive", ok, f"after the run: {run.alive}")


def contract_matches(run: Run, spec: Spec) -> Check:
    """The guardian and the arm on the target both loaded the contract the bench judges
    against: each prints the full SHA-256 of the contract source at startup (M1-J17)."""
    shown: dict[str, str] = {}
    for proc in ("guardian", "arm"):
        line = next((s for s in run.logs if s.startswith(f"{proc} ") and "contract sha256" in s), "")
        shown[proc] = line.split("contract sha256")[-1].split()[0] if line else ""
    ok = all(h == spec.sha256 for h in shown.values())
    detail = (f"guardian and arm both loaded {spec.source.name} (sha256 {spec.sha256})" if ok else
              f"guardian {shown['guardian'] or 'nothing'}, arm {shown['arm'] or 'nothing'}; "
              f"{spec.source.name} is {spec.sha256}")
    return Check("trusted_processes_loaded_this_contract", ok, detail)


def _delta(run: Run, table: dict[str, dict[str, int]], key: str) -> int:
    return table.get("end", {}).get(key, -1) - table.get("begin", {}).get(key, 0)


def trusted_running(run: Run) -> Check:
    """Alive is not enough: a stopped process passes `kill -0` (M1-J11). The watchdog
    counts heartbeats from the guardian and the arm; both advance only while the
    guardian, the arm and the watchdog itself are all running."""
    span_ms = (run.samples[-1].t_ns - run.samples[0].t_ns) / 1e6 if len(run.samples) > 1 else 0.0
    hb = {r: _delta(run, run.wd, f"hb_{r}") for r in ("guardian", "arm")}
    ok = span_ms > 0 and all(h >= 0.8 * span_ms for h in hb.values())
    return Check("trusted_processes_running", ok,
                 f"over {span_ms:.0f} ms of telemetry the watchdog accepted {hb['guardian']} guardian and "
                 f"{hb['arm']} arm heartbeats (1 per ms expected)", {"heartbeats": hb, "span_ms": span_ms})


def no_unexpected_faults(run: Run, extra_ok: frozenset[str] = frozenset()) -> Check:
    """Nothing went wrong that the scenario did not ask for (M1-J11): no watchdog trip,
    no arm rejection, lease expiry, disconnect or watchdog brake, no guardian arm fault,
    foreign pulse or bad message. Counters named in extra_ok are allowed to move."""
    seen = {
        "watchdog trips": _delta(run, run.wd, "trips"),
        "watchdog rejected": _delta(run, run.wd, "rejected"),
        "watchdog deliver errors": _delta(run, run.wd, "deliver_errors"),
        "arm rejected": _delta(run, run.counters, "rejected"),
        "arm lease expiries": _delta(run, run.counters, "lease_expiries"),
        "arm disconnects": _delta(run, run.counters, "disconnects"),
        "arm watchdog brakes": _delta(run, run.counters, "wd_brakes"),
    }
    if run.statuses:
        last = run.statuses[-1]
        seen.update({"guardian arm faults": last.arm_faults, "guardian watchdog trips": last.watchdog_trips,
                     "guardian foreign pulses": last.foreign_pulses,
                     "guardian bad messages": last.bad_messages})
    else:
        seen["guardian status"] = -1
    bad = {k: v for k, v in seen.items() if v != 0 and k not in extra_ok}
    return Check("no_unexpected_faults", not bad,
                 "all zero: " + ", ".join(seen) if not bad else
                 "unexpected: " + ", ".join(f"{k} {v}" for k, v in bad.items()), {"counters": seen})


def telemetry_complete(run: Run) -> Check:
    idx = [s.index for s in run.samples]
    holes = sum(1 for a, b in pairwise(idx) if b != a + 1)
    ok = not run.gaps and holes == 0 and len(idx) > 0
    return Check("telemetry_complete", ok,
                 f"{len(idx)} consecutive samples, {len(run.gaps)} reader gaps, {holes} index holes")


# -------------------------------------------------------------- verdicts

def verdicts_as_expected(run: Run, script: Script, expect: dict[str, tuple[str, str | None]]) -> Check:
    """expect maps a command tag to (verdict name, rule name or None for any rule)."""
    wrong = []
    table = []
    for c, v in zip(script.commands, run.verdicts, strict=False):
        want_v, want_r = expect[c.tag]
        ok = v.seq == c.seq and v.verdict == VERDICT[want_v] and (want_r is None or v.rule == RULE[want_r])
        table.append({"t_ms": c.t_ms, "seq": c.seq, "tag": c.tag, "target_urad": list(c.target),
                      "requested_ms": c.duration_ms, "verdict": VERDICT_NAME.get(v.verdict, str(v.verdict)),
                      "rule": RULE_NAME.get(v.rule, str(v.rule)),
                      "state": STATE_NAME.get(v.state, str(v.state)),
                      "forwarded_ms": v.duration_ms, "ok": ok})
        if not ok:
            wrong.append(f"seq {c.seq} ({c.tag}) got {_vname(v)}, expected {want_v}/{want_r or 'any'}")
    detail = "; ".join(wrong[:5]) or f"all {len(table)} verdicts as expected"
    return Check("verdicts_as_expected", not wrong, detail, {"verdicts": table})


def never_safe_stop(run: Run) -> Check:
    bad = [v.seq for v in run.verdicts if v.state == STATE["SAFE_STOP"]]
    bad += [-1 for s in run.statuses if s.state == STATE["SAFE_STOP"]]
    return Check("guardian_never_in_safe_stop", not bad, "guardian stayed out of SAFE_STOP" if not bad
                 else f"SAFE_STOP seen (seq {bad[:3]})")


# ------------------------------------------------------ independent truth

def joint_limits_never_exceeded(run: Run, spec: Spec) -> Check:
    worst = [float("inf"), float("inf")]   # smallest margin to a limit seen, per joint (µrad)
    first_bad = None
    for s in run.samples:
        for j in range(2):
            m = min(s.pos[j] - spec.joint_min_urad[j], spec.joint_max_urad[j] - s.pos[j])
            worst[j] = min(worst[j], m)
            if m < 0 and first_bad is None:
                first_bad = (s.index, j, s.pos[j])
    detail = (f"closest approach to a limit: shoulder {worst[0] / 1e3:.1f} mrad, "
              f"elbow {worst[1] / 1e3:.1f} mrad"
              if first_bad is None else f"sample {first_bad[0]}: joint {first_bad[1]} at {first_bad[2]} µrad")
    return Check("joint_limits_never_exceeded", first_bad is None, detail,
                 {"min_margin_urad": worst})


def keepout_never_entered(run: Run, spec: Spec) -> Check:
    best = {z.name: float("inf") for z in spec.zones}
    entered = None
    pts = run.samples
    pairs = list(pairwise(pts)) if len(pts) > 1 else [(s, s) for s in pts]
    for a, b in pairs:
        for k in range(KEEPOUT_SUBSTEPS):
            f = k / KEEPOUT_SUBSTEPS
            q0 = a.pos[0] + (b.pos[0] - a.pos[0]) * f
            q1 = a.pos[1] + (b.pos[1] - a.pos[1]) * f
            elbow, tip = kin.elbow_tip(q0, q1)
            for z in spec.zones:
                for name, p in (("elbow", elbow), ("tip", tip)):
                    best[z.name] = min(best[z.name], kin.clearance(p, z))
                    if kin.inside(p, z) and entered is None:
                        entered = (f"{name} inside {z.name} near sample {a.index} "
                                   f"at ({p[0]:.1f}, {p[1]:.1f}) mm")
    detail = entered or "elbow and tip stayed outside; min clearance " + ", ".join(
        f"{n} {d:.1f} mm" for n, d in best.items())
    return Check("keepout_never_entered", entered is None, detail, {"min_clearance_mm": best})


PEAK_WINDOW = 10            # samples per window for the reported peak speed


def speed_never_above_limit(run: Run, spec: Spec) -> tuple[Check, float]:
    """Speed from positions and t_ns only, never from the arm's own velocity field.

    The check is per sample interval, allowing only the arm's integer rounding of the
    two positions. The reported peak is over PEAK_WINDOW-sample windows, where that
    rounding is negligible. Returns the check and the peak (µrad/s)."""
    peak = 0.0
    peak_net = 0.0          # with up to 1 µrad of rounding (two positions, ±0.5 each) taken off
    bad = None
    worst_excess = 0.0
    smp = run.samples
    for a, b in zip(smp, smp[PEAK_WINDOW:], strict=False):
        dt = b.t_ns - a.t_ns
        if dt > 0:
            d = max(abs(b.pos[j] - a.pos[j]) for j in range(2))
            peak = max(peak, d * 1e9 / dt)
            peak_net = max(peak_net, max(d - 1, 0) * 1e9 / dt)
    for a, b in pairwise(smp):
        dt = b.t_ns - a.t_ns
        for j in range(2):
            d = abs(b.pos[j] - a.pos[j])
            if dt <= 0:
                if d != 0 and bad is None:
                    bad = f"samples {a.index}-{b.index}: joint {j} moved {d} µrad in {dt} ns"
                continue
            allowed = spec.vmax_urad_s * dt / 1e9 + POS_ROUNDING_URAD
            worst_excess = max(worst_excess, d - spec.vmax_urad_s * dt / 1e9)
            if d > allowed and bad is None:
                bad = (f"samples {a.index}-{b.index}: joint {j} moved {d} µrad in {dt / 1e6:.3f} ms "
                       f"(limit {allowed:.0f})")
    detail = bad or (f"peak joint speed {peak_net / 1e6:.4f} rad/s net of position rounding "
                     f"({peak / 1e6:.4f} rad/s raw over {PEAK_WINDOW} ms windows), limit "
                     f"{spec.vmax_urad_s / 1e6:.4f} rad/s; largest per-sample step beyond the limit "
                     f"{max(worst_excess, 0.0):.1f} µrad (rounding allowance {POS_ROUNDING_URAD} µrad)")
    return Check("speed_never_above_limit", bad is None, detail,
                 {"peak_urad_s": peak, "peak_net_urad_s": peak_net,
                  "worst_step_excess_urad": worst_excess}), peak


def reached(run: Run, cmd: Command, name: str, until_ms: int | None = None) -> Check:
    """Some sample at cmd's target, after cmd was sent (and before until_ms, if given)."""
    lo = run.t0_ns + cmd.t_ms * 1_000_000
    hi = run.t0_ns + until_ms * 1_000_000 if until_ms is not None else None
    after = [s for s in run.samples if s.t_ns >= lo and (hi is None or s.t_ns <= hi)]
    hit = next((s for s in after
                if all(abs(s.pos[j] - cmd.target[j]) <= REACHED_TOL_URAD for j in range(2))), None)
    detail = (f"target {cmd.target} reached at sample {hit.index}" if hit
              else f"target {cmd.target} never reached")
    return Check(name, hit is not None, detail)


def stayed_at(run: Run, pos: tuple[int, int], from_ms: int, to_ms: int, name: str) -> Check:
    lo, hi = run.t0_ns + from_ms * 1_000_000, run.t0_ns + to_ms * 1_000_000
    win = [s for s in run.samples if lo <= s.t_ns <= hi]
    dev = max((max(abs(s.pos[j] - pos[j]) for j in range(2)) for s in win), default=-1)
    ok = bool(win) and dev <= REACHED_TOL_URAD
    return Check(name, ok, f"{len(win)} samples between {from_ms} and {to_ms} ms, max deviation {dev} µrad "
                 f"from {pos}")


# Targets whose tick spacing is reported but not judged. Owner decision 2026-11-13:
# QEMU proves function, never timing (NFR-T7), so on QEMU the 2-period rule is
# measured and shown, and enforced only on real hardware (the Pi 5). Any other
# target name, known or not, gets the strict rule.
TIMING_NOT_JUDGED = frozenset({"qemu"})
TIMING_POLICY = ("tick spacing on QEMU is measured and reported but not judged (owner decision 2026-11-13: "
                 "QEMU proves function, never timing); the rule of no interval of 2 control periods or more "
                 "is enforced on the Pi 5")


def timing_judged(target: str) -> bool:
    return target not in TIMING_NOT_JUDGED


def no_missed_cycle(run: Run, script: Script) -> Check:
    """No deadline miss, as observable from outside the arm (owner-confirmed 2026-11-13):
    no lease expiry, no guardian-to-arm send fault, the lease valid in every moving
    sample, and no telemetry interval of 2 control periods or more. The interval
    rule is judged on hardware targets only; on QEMU it is measured and reported."""
    ints = [b.t_ns - a.t_ns for a, b in pairwise(run.samples)]
    worst = max(ints, default=0)
    late = sum(1 for i in ints if i >= 2 * PERIOD_NS)
    c0, c1 = run.counters.get("begin", {}), run.counters.get("end", {})
    expiries = c1.get("lease_expiries", -1) - c0.get("lease_expiries", 0)
    faults = run.statuses[-1].arm_faults if run.statuses else -1
    moving_unleased = sum(1 for s in run.samples if s.state == ARM_STATE["MOVING"] and not s.lease_ok)
    strict = timing_judged(run.target)
    ok = expiries == 0 and faults == 0 and moving_unleased == 0 and (late == 0 or not strict)
    spacing = (f"tick spacing: {len(ints)} intervals, longest {worst / 1e6:.3f} ms, "
               f"{late} at 2 periods or more")
    spacing += " (judged)" if strict else f" (reported, not judged: {TIMING_POLICY})"
    return Check("no_deadline_miss", ok,
                 f"lease expiries {expiries}; guardian arm faults {faults}; "
                 f"moving without lease {moving_unleased}; {spacing}",
                 {"longest_interval_ms": worst / 1e6, "intervals_2_periods_or_more": late,
                  "tick_spacing_judged": strict, "target": run.target})


def on_segment(run: Run, p0: tuple[int, int], p1: tuple[int, int], name: str) -> Check:
    """Every sample lies on the joint-space segment p0 -> p1 (within rounding)."""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    n2 = float(dx * dx + dy * dy)
    worst = 0.0
    for s in run.samples:
        u = ((s.pos[0] - p0[0]) * dx + (s.pos[1] - p0[1]) * dy) / n2
        u = min(1.0, max(0.0, u))
        ex, ey = p0[0] + u * dx - s.pos[0], p0[1] + u * dy - s.pos[1]
        worst = max(worst, (ex * ex + ey * ey) ** 0.5)
    ok = worst <= REACHED_TOL_URAD
    return Check(name, ok,
                 f"largest distance from the segment {worst:.1f} µrad over {len(run.samples)} samples")
