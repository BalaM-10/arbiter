"""Tests for the fault bench's judge (FR-B1, FR-B2). Each test is named after its requirement.

The bench decides pass or fail from telemetry with its own kinematics, so these
tests feed it synthetic telemetry that breaks each rule and check that the
judgment fails: a check that cannot fail proves nothing.
"""

from __future__ import annotations

import math
from dataclasses import replace
from itertools import pairwise
from pathlib import Path

import pytest

from arbiter.bench import checks as ck
from arbiter.bench import kinematics as kin
from arbiter.bench.records import Run, Sample, Status, Verdict, parse
from arbiter.bench.scenarios import ALL
from arbiter.bench.script import Script, deg
from arbiter.bench.spec import RULE, VERDICT, Rect, Spec, load_spec

ROOT = Path(__file__).resolve().parents[2]
SPEC: Spec = load_spec(ROOT / "contracts" / "default.yaml")
MS = 1_000_000


def _run(positions: list[tuple[float, float]], dt_ns: int = MS) -> Run:
    """Telemetry with one sample per position, dt_ns apart."""
    run = Run(exit_code=0, done=True)
    for i, (a, b) in enumerate(positions):
        run.samples.append(Sample(i, i * dt_ns, i, (round(a), round(b)), (0, 0), 0, 2, 1))
    return run


def _line(p0: tuple[int, int], p1: tuple[int, int], n: int) -> list[tuple[float, float]]:
    return [(p0[0] + (p1[0] - p0[0]) * k / n, p0[1] + (p1[1] - p0[1]) * k / n) for k in range(n + 1)]


# ------------------------------------------------------------ kinematics

def fr_b2_bench_kinematics_match_the_arm_geometry() -> None:
    elbow, tip = kin.elbow_tip(0, 0)
    assert elbow == pytest.approx((300.0, 0.0)) and tip == pytest.approx((550.0, 0.0))
    elbow, tip = kin.elbow_tip(math.pi / 2 * 1e6, 0)
    assert elbow == pytest.approx((0.0, 300.0), abs=1e-9) and tip == pytest.approx((0.0, 550.0), abs=1e-9)
    elbow, tip = kin.elbow_tip(0, math.pi / 2 * 1e6)
    assert tip == pytest.approx((300.0, 250.0), abs=1e-9)


def fr_b2_bench_reads_limits_from_the_contract_as_written() -> None:
    assert SPEC.joint_max_urad[0] == pytest.approx(150 * math.pi / 180 * 1e6)
    assert SPEC.vmax_urad_s == pytest.approx(57.29578 * math.pi / 180 * 1e6)
    assert (SPEC.zones[0].x_min, SPEC.zones[0].y_max) == (200.0, 350.0)


# ---------------------------------------------- each check can fail

def fr_b2_keepout_check_fails_when_the_arm_enters_the_zone() -> None:
    run = _run(_line((deg(15), 0), (deg(40), 0), 200))
    c = ck.keepout_never_entered(run, SPEC)
    assert not c.passed and "inside operator_hand_area" in c.detail


def fr_b2_keepout_check_catches_a_crossing_between_two_samples() -> None:
    # Two samples, both outside the area, whose joint-space segment sweeps through it.
    run = _run([(deg(15), 0), (deg(75), 0)])
    for s in run.samples:
        e, t = kin.elbow_tip(*s.pos)
        assert not kin.inside(e, SPEC.zones[0]) and not kin.inside(t, SPEC.zones[0])
    assert not ck.keepout_never_entered(run, SPEC).passed


B1_POSE = (deg(25), deg(25))     # midpoint review B1: elbow and tip clear, forearm across the area


def fr_b2_keepout_check_fails_when_the_forearm_crosses_the_zone() -> None:
    # Review B1: at the end pose the elbow and the tip are both outside the area,
    # but the forearm between them lies across it.
    elbow, tip = kin.elbow_tip(*B1_POSE)
    assert not kin.inside(elbow, SPEC.zones[0]) and not kin.inside(tip, SPEC.zones[0])
    run = _run(_line((0, 0), B1_POSE, 200))
    c = ck.keepout_never_entered(run, SPEC)
    assert not c.passed and "forearm inside" in c.detail


def fr_b2_keepout_check_fails_when_the_upper_arm_crosses_a_zone() -> None:
    small = replace(SPEC, zones=(Rect("near_base", 100.0, 150.0, 30.0, 60.0),))
    run = _run(_line((0, 0), (deg(20), 0), 100))
    c = ck.keepout_never_entered(run, small)
    assert not c.passed and "upper arm inside" in c.detail


def fr_b2_link_clearance_is_exact_for_segments() -> None:
    r = Rect("r", 0.0, 10.0, 0.0, 10.0)
    assert kin.segment_clearance((-5.0, 5.0), (15.0, 5.0), r) == 0.0        # straight through
    assert kin.segment_clearance((-5.0, 13.0), (15.0, 13.0), r) == pytest.approx(3.0)  # above
    assert kin.segment_clearance((13.0, 14.0), (20.0, 30.0), r) == pytest.approx(5.0)  # off a corner
    assert kin.segment_clearance((-3.0, 8.0), (8.0, -3.0), r) == 0.0        # clips a corner only
    assert kin.segment_clearance((-6.0, 4.0), (4.0, -6.0), r) == pytest.approx(math.sqrt(2.0))


def fr_b2_f5_denies_a_move_whose_forearm_would_cross_the_area() -> None:
    # F5 must exercise B1: a denied target whose elbow and tip are clear at the end.
    s = ALL["F5"].build(SPEC)
    targets = [c.target for c in s.commands if c.tag == "violation"]
    assert B1_POSE in targets
    assert not ck.keepout_never_entered(_run([B1_POSE]), SPEC).passed   # the pose itself crosses


def fr_b2_keepout_check_passes_beside_the_zone() -> None:
    assert ck.keepout_never_entered(_run(_line((0, 0), (deg(15), 0), 100)), SPEC).passed


def fr_b2_speed_check_fails_above_the_limit() -> None:
    run = _run([(0, 0), (1100, 0)])                      # 1.1 rad/s over 1 ms
    c, _ = ck.speed_never_above_limit(run, SPEC)
    assert not c.passed


def fr_b2_speed_check_allows_only_integer_rounding() -> None:
    assert ck.speed_never_above_limit(_run([(0, 0), (1002, 0)]), SPEC)[0].passed
    assert not ck.speed_never_above_limit(_run([(0, 0), (1003, 0)]), SPEC)[0].passed


def fr_b2_speed_check_uses_sample_times_not_nominal_period() -> None:
    # 1500 µrad is fine over 1.5 ms, too fast over 1 ms.
    assert ck.speed_never_above_limit(_run([(0, 0), (1500, 0)], dt_ns=1_500_000), SPEC)[0].passed
    assert not ck.speed_never_above_limit(_run([(0, 0), (1500, 0)], dt_ns=MS), SPEC)[0].passed


def fr_b2_joint_limit_check_fails_one_step_past_the_limit() -> None:
    lim = SPEC.joint_min_urad[0]
    assert ck.joint_limits_never_exceeded(_run([(math.ceil(lim), 0)]), SPEC).passed
    assert not ck.joint_limits_never_exceeded(_run([(math.floor(lim) - 1, 0)]), SPEC).passed


def fr_b2_telemetry_check_fails_on_a_lost_sample() -> None:
    run = _run([(0, 0)] * 5)
    assert ck.telemetry_complete(run).passed
    del run.samples[2]
    assert not ck.telemetry_complete(run).passed
    run = _run([(0, 0)] * 5)
    run.gaps.append((7, 1))
    assert not ck.telemetry_complete(run).passed


def fr_b2_on_segment_check_fails_when_the_arm_heads_elsewhere() -> None:
    a, b = (-200_000, 100_000), (-600_000, -300_000)
    assert ck.on_segment(_run(_line((0, 0), a, 50)), (0, 0), a, "x").passed
    assert not ck.on_segment(_run(_line((0, 0), a, 50) + _line(a, b, 5)[1:]), (0, 0), a, "x").passed


def fr_b2_verdict_check_fails_on_the_wrong_rule() -> None:
    s = Script()
    s.send((deg(-160), 0), 1000, "violation", moves=False)
    run = Run(exit_code=0, done=True)
    run.verdicts.append(Verdict(0, 1, 0, 1, 0, VERDICT["DENY"], RULE["KEEPOUT"], 2, 0, (0, 0), 0, 1000))
    assert not ck.verdicts_as_expected(run, s, {"violation": ("DENY", "JOINT_LIMIT")}).passed
    run.verdicts[0] = Verdict(0, 1, 0, 1, 0, VERDICT["DENY"], RULE["JOINT_LIMIT"], 2, 0, (0, 0), 0, 1000)
    assert ck.verdicts_as_expected(run, s, {"violation": ("DENY", "JOINT_LIMIT")}).passed


def _cycle_run(target: str, *, late: bool = False, expiries: int = 0, arm_faults: int = 0) -> Run:
    run = _run([(0, 0)] * 4)
    run.target = target
    run.counters = {"begin": {"lease_expiries": 0}, "end": {"lease_expiries": expiries}}
    run.statuses.append(Status(0, 2, 0, 0, arm_faults, 0, 0, 0, 0))
    if late:
        run.samples[3] = Sample(3, 2 * MS + 2 * MS, 3, (0, 0), (0, 0), 0, 2, 1)   # a 2-period gap
    return run


def f1_tick_spacing_is_strict_on_the_pi5_and_on_unknown_targets() -> None:
    for target in ("pi5", "some-new-board", ""):
        assert ck.no_missed_cycle(_cycle_run(target), Script()).passed
        c = ck.no_missed_cycle(_cycle_run(target, late=True), Script())
        assert not c.passed and "(judged)" in c.detail and c.data["tick_spacing_judged"] is True


def f1_tick_spacing_on_qemu_is_reported_with_the_reason_not_judged() -> None:
    for target in ("qemu", "qemu_secpol"):                            # both QEMU images, policy off and on
        c = ck.no_missed_cycle(_cycle_run(target, late=True), Script())
        assert c.passed and c.data["tick_spacing_judged"] is False
        assert c.data["intervals_2_periods_or_more"] == 1             # still measured and shown
        assert "not judged" in c.detail and "enforced on the Pi 5" in c.detail


def f1_lease_expiry_or_send_fault_fails_on_every_target() -> None:
    for target in ("qemu", "pi5"):
        assert not ck.no_missed_cycle(_cycle_run(target, expiries=1), Script()).passed
        assert not ck.no_missed_cycle(_cycle_run(target, arm_faults=1), Script()).passed
        moving_unleased = _cycle_run(target)
        moving_unleased.samples[1] = Sample(1, MS, 1, (0, 0), (0, 0), 0, 2, 0)   # MOVING, lease_ok 0
        assert not ck.no_missed_cycle(moving_unleased, Script()).passed


# ------------------------------------------------------- scenario scripts

@pytest.mark.parametrize("sid", sorted(ALL))
def fr_b1_scenario_scripts_are_valid_for_arb_inject(sid: str) -> None:
    script = ALL[sid].build(SPEC)
    lines = script.text().splitlines()
    assert lines[-1].startswith("E ") and len(lines) <= 2048
    times = [int(line.split()[1]) for line in lines]
    assert times == sorted(times)
    for c in script.commands:
        assert c.duration_ms >= 1
    # The planner keeps talking inside the silence timeout, and under the contract's rate.
    starts = [c.t_ms for c in script.commands]
    assert max(b - a for a, b in pairwise(starts)) < SPEC.silence_timeout_ms
    assert max(sum(1 for t in starts if t0 <= t < t0 + 1000) for t0 in starts) <= 50


@pytest.mark.parametrize("sid", sorted(ALL))
def fr_b2_every_scenario_states_its_pass_condition(sid: str) -> None:
    m = ALL[sid]
    assert sid == m.ID and m.TITLE and len(m.PASS_CONDITION) > 40


def fr_b2_parser_reads_arb_inject_output() -> None:
    h = SPEC.sha256
    text = ("ARB-INJECT 1\nCOUNTERS begin rejected 0 lease_expiries 0 disconnects 0 wd_brakes 0 wd_lost 0 "
            "bound_pid 5\n"
            "WD begin trips 0 rejected 0 hb_guardian 100 hb_arm 200 deliver_errors 0 beat_errors 0\n"
            "V 10 20 7 1 0 1 0 2 0 -5 6 200 200\nS 30 2 0 1 0 9 0 0 3 1\nT 7 11 99 -5 6 0 0 1 1 1\n"
            "GAP 9 2\nALIVE watchdog 1 arm 1 guardian 0\n"
            f"LOG guardian arb-guardian: ready on /dev/arbiter, priority 64, contract sha256 {h}\n"
            f"LOG arm arb-arm: ready as arbiter_arm, priority 70, contract sha256 {h}\nDONE x\n")
    run = parse(text, 0)
    assert run.verdicts[0].target == (-5, 6) and run.verdicts[0].rule == 0
    assert run.samples[0].pos == (-5, 6) and run.statuses[0].ticks == 9
    assert run.statuses[0].foreign_pulses == 3 and run.wd["begin"]["hb_arm"] == 200
    assert run.statuses[0].watchdog_lost == 1 and run.counters["begin"]["bound_pid"] == 5
    assert run.gaps == [(9, 2)] and run.alive["guardian"] == 0 and run.done
    assert not ck.trusted_alive(run).passed
    assert ck.contract_matches(run, SPEC).passed


def fr_b2_parser_reads_trip_causes_and_the_system_log() -> None:
    text = ("COUNTERS end rejected 0 lease_expiries 1 disconnects 0 wd_brakes 1 wd_lost 1 bound_pid 5 "
            "wd_lost_cause 2\n"
            "WD end trips 2 rejected 9 hb_guardian 100 hb_arm 200 deliver_errors 0 beat_errors 0 "
            "last_role 2 last_cause 2 last_pid 77\n"
            "S 30 3 3 1 0 9 0 0 0 1 2\n"
            "SLOG watchdog Nov 17 02:37:32.753 arb_watchdog.7 main 7 trip: the guardian disconnected\n"
            "SLOG arm unavailable: slog2info: no permission\n")
    run = parse(text, 0)
    assert run.counters["end"]["wd_lost_cause"] == 2 and run.counters["end"]["bound_pid"] == 5
    assert run.wd["end"]["last_role"] == 2 and run.wd["end"]["last_pid"] == 77
    assert run.statuses[0].watchdog_lost == 1 and run.statuses[0].watchdog_lost_cause == 2
    assert run.slog == ["watchdog Nov 17 02:37:32.753 arb_watchdog.7 main 7 trip: the guardian disconnected",
                        "arm unavailable: slog2info: no permission"]
    old = parse("WD end trips 0 rejected 0 hb_guardian 1 hb_arm 1 deliver_errors 0 beat_errors 0\n"
                "S 30 2 0 1 0 9 0 0 3 1\n", 0)               # the format before 2026-11-16 still parses
    assert old.wd["end"]["trips"] == 0 and old.statuses[0].watchdog_lost_cause == 0


def fr_b2_contract_check_needs_the_full_hash_from_guardian_and_arm() -> None:
    h = SPEC.sha256
    both = f"LOG guardian x contract sha256 {h}\nLOG arm y contract sha256 {h}\n"
    assert ck.contract_matches(parse(both, 0), SPEC).passed
    assert not ck.contract_matches(parse(f"LOG guardian x contract sha256 {h}\n", 0), SPEC).passed
    prefix_only = f"LOG guardian x contract sha256 {h[:8]}...\nLOG arm y contract sha256 {h}\n"
    assert not ck.contract_matches(parse(prefix_only, 0), SPEC).passed


def _fault_run() -> Run:
    run = _run([(0, 0)] * 101)                       # 100 ms of telemetry
    zero_arm = {"rejected": 0, "lease_expiries": 0, "disconnects": 0, "wd_brakes": 0, "wd_lost": 0}
    zero_wd = {"trips": 0, "rejected": 0, "deliver_errors": 0, "beat_errors": 0,
               "hb_guardian": 0, "hb_arm": 0}
    run.counters = {"begin": dict(zero_arm), "end": dict(zero_arm)}
    run.wd = {"begin": dict(zero_wd), "end": dict(zero_wd, hb_guardian=100, hb_arm=100)}
    run.statuses.append(Status(0, 2, 0, 5, 0, 100, 0, 0, 0))
    return run


def fr_b2_unexpected_fault_counters_fail_the_baseline() -> None:
    assert ck.no_unexpected_faults(_fault_run()).passed
    for table, key in (("wd", "trips"), ("wd", "beat_errors"), ("counters", "rejected"),
                       ("counters", "wd_brakes"), ("counters", "wd_lost")):
        run = _fault_run()
        getattr(run, table)["end"][key] = 1
        assert not ck.no_unexpected_faults(run).passed, key
    run = _fault_run()
    run.statuses[0] = Status(0, 2, 0, 5, 0, 100, 0, 0, 7)  # foreign pulses
    assert not ck.no_unexpected_faults(run).passed
    assert ck.no_unexpected_faults(run, frozenset({"guardian foreign pulses"})).passed
    run = _fault_run()
    run.statuses[0] = Status(0, 2, 0, 5, 0, 100, 0, 0, 0, 1)  # the guardian lost the watchdog
    assert not ck.no_unexpected_faults(run).passed


def fr_b2_unexpected_faults_name_who_tripped_and_why() -> None:
    run = _fault_run()
    run.wd["end"].update(trips=1, last_role=1, last_cause=2, last_pid=42)
    run.counters["end"].update(wd_lost=1, wd_lost_cause=2)
    run.statuses[0] = Status(0, 3, 3, 5, 0, 100, 0, 0, 0, 1, 2)
    c = ck.no_unexpected_faults(run)
    assert not c.passed
    assert "the watchdog tripped the guardian (pid 42): stuck (no heartbeat for 10 ms)" in c.detail
    assert "the arm lost the watchdog: no beat from the watchdog for 10 ticks" in c.detail
    assert "the guardian lost the watchdog: no beat from the watchdog for 10 ticks" in c.detail
    assert c.data["causes"] and ck.no_unexpected_faults(_fault_run()).data["causes"] == ""


def fr_b2_a_stopped_trusted_process_fails_the_running_check() -> None:
    assert ck.trusted_running(_fault_run()).passed
    run = _fault_run()
    run.wd["end"]["hb_guardian"] = 3                    # alive (kill -0 passes) but not running
    assert not ck.trusted_running(run).passed


def fr_b3_bench_exits_nonzero_when_a_scenario_fails(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    import subprocess

    from arbiter.bench import runner

    def fake_ssh(target: str, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(args, 0, stdout="", stderr="")   # target gave no records

    monkeypatch.setattr(runner, "_ssh", fake_ssh)
    rc = runner.run_bench("qemu", ROOT / "contracts" / "default.yaml", ["F9"], tmp_path, deploy=False)
    assert rc == 1
    reports = list(tmp_path.glob("*/*/report.json"))
    assert len(reports) == 1 and '"passed": false' in reports[0].read_text()
    assert (reports[0].parent / "report.html").exists()
    assert (reports[0].parent / "raw" / "F9.slog2.txt").exists()       # the system log is kept per scenario


# ------------------------------------------------- the target's evidence log

def _evlog(decisions: list[tuple[int, int, int]], end: bool = True, lost: int = 0) -> tuple[list[str], str]:
    """An arb-logd log (docs/EVIDENCE_LOG.md) for (seq, verdict, rule) decisions, hashed
    here with hashlib; returns the lines and the anchor."""
    import hashlib

    bodies = [f"H format 1 contract {SPEC.sha256} cycles_per_sec 1 start_ns 1"]
    idx = 0
    if lost:
        bodies.append(f"G lost {lost} from_idx 0 to_idx {lost - 1}")
        idx = lost
    for seq, verdict, rule in decisions:
        bodies.append(f"R idx {idx} t_ns 1 seq {seq} target 0 0 dur 100 verdict {verdict} rule {rule} "
                      f"before 2 after 2 flags 0 fwd 0 0 100 latency_cycles 9")
        idx += 1
    if end:
        bodies.append(f"E records {len(decisions)} lost {lost}")
    lines, prev = [], "0" * 64
    for n, body in enumerate(bodies):
        tag, _, rest = body.partition(" ")
        text = f"{tag} {n} {rest} prev {prev}"
        prev = hashlib.sha256(text.encode()).hexdigest()
        lines.append(f"{text} hash {prev}")
    return lines, prev


def _verdict(seq: int, verdict: int, rule: int, rc: int = 0) -> Verdict:
    return Verdict(0, 0, 0, seq, rc, verdict, rule, 2, 0, (0, 0), 100, 100)


def fr_e4_bench_checks_the_targets_evidence_log_with_its_anchor() -> None:
    run = Run()
    run.evlog, run.evanchor = _evlog([(1, 1, 0), (2, 3, 4)])
    assert ck.evidence_log_verifies(run, SPEC).passed
    edited = Run(evlog=list(run.evlog), evanchor=run.evanchor)
    edited.evlog[2] = edited.evlog[2].replace(" verdict 3 ", " verdict 1 ")
    assert not ck.evidence_log_verifies(edited, SPEC).passed
    truncated = Run(evlog=run.evlog[:-1], evanchor=run.evanchor)        # the E line removed
    assert not ck.evidence_log_verifies(truncated, SPEC).passed
    no_anchor = Run(evlog=list(run.evlog))
    assert not ck.evidence_log_verifies(no_anchor, SPEC).passed
    lossy = Run()
    lossy.evlog, lossy.evanchor = _evlog([(1, 1, 0)], lost=3)
    assert not ck.evidence_log_verifies(lossy, SPEC).passed               # the bench never expects a loss
    assert not ck.evidence_log_verifies(Run(), SPEC).passed


def fr_e1_bench_checks_every_verdict_is_in_the_evidence_log() -> None:
    run = Run()
    run.verdicts = [_verdict(1, 1, 0), _verdict(2, 3, 4), _verdict(3, 0, 0, rc=1)]   # the last: not decided
    run.evlog, run.evanchor = _evlog([(1, 1, 0), (2, 3, 4)])
    assert ck.evidence_log_has_every_verdict(run).passed
    missing = Run(verdicts=run.verdicts)
    missing.evlog, _ = _evlog([(1, 1, 0)])
    assert not ck.evidence_log_has_every_verdict(missing).passed
    wrong = Run(verdicts=run.verdicts)
    wrong.evlog, _ = _evlog([(1, 1, 0), (2, 1, 0)])
    assert not ck.evidence_log_has_every_verdict(wrong).passed
