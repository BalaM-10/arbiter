"""Checks shared by the scenarios. Each returns a Check with the evidence that decided it.

Telemetry checks are the independent truth: they use only the arm's samples
(position and the time it was computed for), the contract as written, and the
bench's own kinematics.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from itertools import pairwise
from typing import Any

from .. import evidence
from . import kinematics as kin
from .records import Run, Verdict
from .script import Command, Script
from .spec import ARM_STATE, PERIOD_NS, RULE, RULE_NAME, STATE, STATE_NAME, STOP, VERDICT, VERDICT_NAME, Spec

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


def evidence_log_verifies(run: Run, spec: Spec) -> Check:
    """FR-E2, FR-E4, FR-C6 on the target's own log: arb-logd's chain recomputed here with
    hashlib, against the anchor it published, ended cleanly, nothing lost, and the header
    names the contract the bench judges against."""
    text = "".join(f"{x}\n" for x in run.evlog)
    r = evidence.verify(text, anchor=run.evanchor or None)
    problems = list(r.errors)
    if not run.evlog:
        problems = ["no evidence log from the target"]
    else:
        if not run.evanchor:
            problems.append("no anchor published")
        if r.ok and not r.terminated:
            problems.append("no end record (the logger did not stop cleanly)")
        if r.ok and r.lost:
            problems.append(f"{r.lost} records declared lost")
        if r.ok and r.contract != spec.sha256:
            problems.append(f"header contract {r.contract}, {spec.source.name} is {spec.sha256}")
    ok = not problems
    detail = (f"{r.lines} lines, {r.records} decisions, chain and anchor recomputed with hashlib, "
              "contract matches" if ok else "; ".join(problems))
    return Check("evidence_log_verifies", ok, detail, {"lines": r.lines, "records": r.records, "lost": r.lost,
                                                       "last_hash": r.last_hash})


def evidence_log_has_every_verdict(run: Run) -> Check:
    """FR-E1: every command the guardian decided for arb-inject is in the evidence log, in
    order, with the verdict and rule arb-inject received."""
    r = evidence.verify("".join(f"{x}\n" for x in run.evlog))
    decided = [v for v in run.verdicts if v.rc == 0]
    logged = r.decisions
    bad: list[str] = []
    if len(logged) != len(decided):
        bad.append(f"{len(decided)} decided, {len(logged)} logged")
    for v, ln in zip(decided, logged, strict=False):
        if (ln.int("seq"), ln.int("verdict"), ln.int("rule")) != (v.seq, v.verdict, v.rule):
            bad.append(f"seq {v.seq}: replied {v.verdict}/{v.rule}, logged seq {ln.int('seq')} "
                       f"{ln.int('verdict')}/{ln.int('rule')}")
            break
    ok = r.ok and not bad
    detail = (f"all {len(decided)} decisions logged in order with their verdict and rule" if ok
              else "; ".join(bad or r.errors))
    return Check("evidence_log_has_every_verdict", ok, detail)


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


# The C enums in qnx/common/arb_watchdog_proto.h: ARB_WD_ROLE_*, ARB_WD_CAUSE_*, ARB_WD_LOST_*.
WD_ROLE = {1: "guardian", 2: "arm"}
WD_CAUSE = {1: "died", 2: "stuck (no heartbeat for 10 ms)", 3: "disconnected"}
WD_LOST = {1: "its connection to the watchdog died", 2: "no beat from the watchdog for 10 ticks"}


def fault_causes(run: Run) -> str:
    """Who stopped the system and why, from the counters arb-inject prints at the end:
    the watchdog's last trip, and each client's first loss of the watchdog."""
    parts: list[str] = []
    wd = run.wd.get("end", {})
    if _delta(run, run.wd, "trips") > 0 and "last_role" in wd:
        role, cause = wd["last_role"], wd.get("last_cause", 0)
        parts.append(f"the watchdog tripped the {WD_ROLE.get(role, role)} "
                     f"(pid {wd.get('last_pid', -1)}): {WD_CAUSE.get(cause, cause)}")
    arm = run.counters.get("end", {})
    if _delta(run, run.counters, "wd_lost") > 0 and arm.get("wd_lost_cause", 0):
        parts.append(f"the arm lost the watchdog: {WD_LOST.get(arm['wd_lost_cause'], arm['wd_lost_cause'])}")
    if run.statuses and run.statuses[-1].watchdog_lost and run.statuses[-1].watchdog_lost_cause:
        c = run.statuses[-1].watchdog_lost_cause
        parts.append(f"the guardian lost the watchdog: {WD_LOST.get(c, c)}")
    return "; ".join(parts)


def no_unexpected_faults(run: Run, extra_ok: frozenset[str] = frozenset()) -> Check:
    """Nothing went wrong that the scenario did not ask for (M1-J11): no watchdog trip or
    beat error, no arm rejection, lease expiry, disconnect, watchdog brake or lost
    watchdog, no guardian arm fault, foreign pulse, bad message or lost watchdog.
    Counters named in extra_ok are allowed to move."""
    seen = {
        "watchdog trips": _delta(run, run.wd, "trips"),
        "watchdog rejected": _delta(run, run.wd, "rejected"),
        "watchdog deliver errors": _delta(run, run.wd, "deliver_errors"),
        "watchdog beat errors": _delta(run, run.wd, "beat_errors"),
        "arm rejected": _delta(run, run.counters, "rejected"),
        "arm lease expiries": _delta(run, run.counters, "lease_expiries"),
        "arm disconnects": _delta(run, run.counters, "disconnects"),
        "arm watchdog brakes": _delta(run, run.counters, "wd_brakes"),
        "arm watchdog lost": _delta(run, run.counters, "wd_lost"),
    }
    if run.statuses:
        last = run.statuses[-1]
        seen.update({"guardian arm faults": last.arm_faults, "guardian watchdog trips": last.watchdog_trips,
                     "guardian foreign pulses": last.foreign_pulses,
                     "guardian watchdog lost": last.watchdog_lost,
                     "guardian bad messages": last.bad_messages})
    else:
        seen["guardian status"] = -1
    bad = {k: v for k, v in seen.items() if v != 0 and k not in extra_ok}
    causes = fault_causes(run)
    if not bad:
        detail = "all zero: " + ", ".join(seen)
    else:
        detail = "unexpected: " + ", ".join(f"{k} {v}" for k, v in bad.items())
        if causes:
            detail += f" ({causes})"
    return Check("no_unexpected_faults", not bad, detail, {"counters": seen, "causes": causes})


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
    """No point of either link (upper arm, forearm) touches a zone, at any sample or substep."""
    best = {z.name: float("inf") for z in spec.zones}
    entered = None
    pts = run.samples
    pairs = list(pairwise(pts)) if len(pts) > 1 else [(s, s) for s in pts]
    for a, b in pairs:
        for k in range(KEEPOUT_SUBSTEPS):
            f = k / KEEPOUT_SUBSTEPS
            q0 = a.pos[0] + (b.pos[0] - a.pos[0]) * f
            q1 = a.pos[1] + (b.pos[1] - a.pos[1]) * f
            for name, p0, p1 in kin.links(q0, q1):
                for z in spec.zones:
                    d = kin.segment_clearance(p0, p1, z)
                    best[z.name] = min(best[z.name], d)
                    if d == 0.0 and entered is None:
                        entered = (f"{name} inside {z.name} near sample {a.index} "
                                   f"(from ({p0[0]:.1f}, {p0[1]:.1f}) to ({p1[0]:.1f}, {p1[1]:.1f}) mm)")
    detail = entered or "both links stayed outside; min clearance " + ", ".join(
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
# target name, known or not, gets the strict rule. qemu_secpol is the same QEMU
# target with the security policy loaded (M2 P5).
TIMING_NOT_JUDGED = frozenset({"qemu", "qemu_secpol"})
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


# ------------------------------------------- M2 P7: F2, F13, F14, F15

def decisions(run: Run) -> list[dict[str, int]]:
    """The guardian's own decision records (its evidence log's R lines), in order."""
    out: list[dict[str, int]] = []
    for line in run.evlog:
        f = line.split()
        if not f or f[0] != "R":
            continue
        keys = ("seq", "t_ns", "verdict", "rule", "before", "after")
        d = {k: int(f[f.index(k) + 1]) for k in keys if k in f}
        d["line"] = int(f[1])
        out.append(d)
    return out


def hold_recorded_before(run: Run, seq: int) -> Check:
    """F2: when the planner came back with seq, the guardian's own record says it was in HOLD."""
    d = next((x for x in decisions(run) if x.get("seq") == seq), None)
    ok = d is not None and d.get("before") == STATE["HOLD"]
    detail = (f"no decision for seq {seq} in the evidence log" if d is None else
              f"decision for seq {seq}: state before {STATE_NAME.get(d.get('before', -1), d.get('before'))}, "
              f"after {STATE_NAME.get(d.get('after', -1), d.get('after'))} (the guardian's evidence log)")
    return Check("hold_recorded_by_the_guardian", ok, detail)


def stopped_short(run: Run, target: tuple[int, int], from_ms: int, to_ms: int, name: str) -> Check:
    """F2: between from_ms and to_ms the arm stands still, and not at target."""
    lo, hi = run.t0_ns + from_ms * 1_000_000, run.t0_ns + to_ms * 1_000_000
    win = [s for s in run.samples if lo <= s.t_ns <= hi]
    if not win:
        return Check(name, False, f"no sample between {from_ms} and {to_ms} ms")
    p0 = win[0].pos
    drift = max(max(abs(s.pos[j] - p0[j]) for j in range(2)) for s in win)
    gap = max(abs(p0[j] - target[j]) for j in range(2))
    ok = drift <= REACHED_TOL_URAD and gap > REACHED_TOL_URAD
    return Check(name, ok, f"{len(win)} samples between {from_ms} and {to_ms} ms: moved at most "
                 f"{drift} µrad, standing {gap} µrad short of the target {target}")


def _action(run: Run, name: str) -> int | None:
    return next((a.t_ns for a in run.actions if a.name == name and a.rc == 0), None)


def braked_within(run: Run, action: str, cycles: int, name: str) -> Check:
    """F13, F14: after the action the arm starts braking (its own state, from telemetry:
    STOPPING, decelerating along its segment, or BRAKED) within `cycles` control
    cycles, comes to rest BRAKED, and never moves again. The cycle count is judged on
    hardware targets, measured and reported on QEMU (owner decision 2026-11-13); the
    time to rest is reported."""
    t = _action(run, action)
    if t is None:
        return Check(name, False, f"no successful '{action}' action recorded")
    after = [s for s in run.samples if s.t_ns >= t]
    braking = (ARM_STATE["STOPPING"], ARM_STATE["BRAKED"])
    k0 = next((i for i, s in enumerate(after) if s.state in braking), None)
    k = next((i for i, s in enumerate(after) if s.state == ARM_STATE["BRAKED"]), None)
    if k0 is None or k is None:
        return Check(name, False, f"the arm never braked after the {action} ({len(after)} samples)")
    b = after[k]
    n0 = -(-(after[k0].t_ns - t) // PERIOD_NS)
    n = -(-(b.t_ns - t) // PERIOD_NS)
    drift = max((max(abs(s.pos[j] - b.pos[j]) for j in range(2)) for s in after[k:]), default=0)
    unbraked = sum(1 for s in after[k:] if s.state != ARM_STATE["BRAKED"])
    left = sum(1 for s in after[k0:k] if s.state not in braking)
    strict = timing_judged(run.target)
    ok = drift <= REACHED_TOL_URAD and unbraked == 0 and left == 0 and (n0 <= cycles or not strict)
    timing = (f"braking began {n0} control cycles after the {action} (limit {cycles}"
              + (", judged)" if strict else ", reported, not judged: QEMU proves function, never timing)"))
    return Check(name, ok, f"{timing}; at rest and braked after {n} cycles; afterwards moved at most "
                 f"{drift} µrad, {unbraked} samples not braked",
                 {"cycles": n0, "rest_cycles": n, "judged": strict})


WD_CAUSE_SHORT = {1: "died", 2: "stuck", 3: "disconnected"}


def watchdog_recorded(run: Run, role: str, causes: tuple[str, ...]) -> Check:
    """F13: the watchdog tripped for this role, with one of these causes."""
    wd = run.wd.get("end", {})
    trips = _delta(run, run.wd, "trips")
    got_role = WD_ROLE.get(wd.get("last_role", 0), "none")
    got_cause = WD_CAUSE_SHORT.get(wd.get("last_cause", 0), "none")
    ok = trips >= 1 and got_role == role and got_cause in causes
    return Check("watchdog_recorded_the_cause", ok,
                 f"watchdog trips {trips}; last: the {got_role}, {got_cause} (pid {wd.get('last_pid', -1)})")


def safe_stop_by(run: Run, cause: str) -> Check:
    """F14: the guardian's last status is SAFE_STOP with this cause."""
    if not run.statuses:
        return Check("safe_stop_with_its_cause", False, "no guardian status recorded")
    st = run.statuses[-1]
    ok = st.state == STATE["SAFE_STOP"] and st.stop_cause == STOP[cause]
    names = {v: k for k, v in STOP.items()}
    return Check("safe_stop_with_its_cause", ok, f"guardian {STATE_NAME.get(st.state, st.state)}, cause "
                 f"{names.get(st.stop_cause, st.stop_cause)} (expected SAFE_STOP, {cause})")


def tampering_detected(run: Run) -> Check:
    """F15: change one byte of this scenario's own evidence log (in a decision record's
    duration) and `arbiter evidence verify` must fail, naming that line first."""
    recs = [x for x in run.evlog if x.startswith("R ")]
    if not recs or not run.evanchor:
        return Check("tampering_detected", False, "no evidence log to change")
    victim = recs[1] if len(recs) > 1 else recs[0]
    n = int(victim.split()[1])
    i = victim.index(" dur ") + len(" dur ")
    changed = victim[:i] + ("2" if victim[i] != "2" else "3") + victim[i + 1:]
    text = "".join(f"{changed if x is victim else x}\n" for x in run.evlog)
    v = evidence.verify(text, anchor=run.evanchor)
    first = v.errors[0] if v.errors else ""
    ok = not v.ok and first.startswith(f"line {n}")
    return Check("tampering_detected", ok,
                 f"changed one byte of line {n} ('dur {victim[i]}' -> 'dur {changed[i]}'): "
                 + (f"verify fails first at: {first}" if v.errors else "verify still passes"))
