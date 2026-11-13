"""Tests for the fault bench's judge (FR-B1, FR-B2). Each test is named after its requirement.

The bench decides pass or fail from telemetry with its own kinematics, so these
tests feed it synthetic telemetry that breaks each rule and check that the
judgment fails: a check that cannot fail proves nothing.
"""

from __future__ import annotations

import math
from itertools import pairwise
from pathlib import Path

import pytest

from arbiter.bench import checks as ck
from arbiter.bench import kinematics as kin
from arbiter.bench.records import Run, Sample, Status, Verdict, parse
from arbiter.bench.scenarios import ALL
from arbiter.bench.script import Script, deg
from arbiter.bench.spec import RULE, VERDICT, Spec, load_spec

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

def fr_b2_keepout_check_fails_when_the_elbow_enters_the_zone() -> None:
    run = _run(_line((deg(15), 0), (deg(40), 0), 200))
    c = ck.keepout_never_entered(run, SPEC)
    assert not c.passed and "elbow inside" in c.detail


def fr_b2_keepout_check_catches_a_crossing_between_two_samples() -> None:
    # Two samples, both outside the area, whose joint-space segment sweeps through it.
    run = _run([(deg(15), 0), (deg(75), 0)])
    for s in run.samples:
        e, t = kin.elbow_tip(*s.pos)
        assert not kin.inside(e, SPEC.zones[0]) and not kin.inside(t, SPEC.zones[0])
    assert not ck.keepout_never_entered(run, SPEC).passed


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


def fr_b2_missed_cycle_check_fails_on_a_long_interval() -> None:
    run = _run([(0, 0)] * 4)
    run.counters = {"begin": {"lease_expiries": 0}, "end": {"lease_expiries": 0}}
    s = Script()
    run.statuses.append(Status(0, 2, 0, 0, 0, 0, 0, 0))
    assert ck.no_missed_cycle(run, s).passed
    run.samples[3] = Sample(3, 2 * MS + 2 * MS, 3, (0, 0), (0, 0), 0, 2, 1)
    assert not ck.no_missed_cycle(run, s).passed


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
    text = ("ARB-INJECT 1\nCOUNTERS begin rejected 0 lease_expiries 0 disconnects 0 wd_brakes 0 bound_pid 5\n"
            "V 10 20 7 1 0 1 0 2 0 -5 6 200 200\nS 30 2 0 1 0 9 0 0\nT 7 11 99 -5 6 0 0 1 1 1\n"
            "GAP 9 2\nALIVE watchdog 1 arm 1 guardian 0\nLOG guardian contract sha256 265a6f18...\nDONE x\n")
    run = parse(text, 0)
    assert run.verdicts[0].target == (-5, 6) and run.verdicts[0].rule == 0
    assert run.samples[0].pos == (-5, 6) and run.statuses[0].ticks == 9
    assert run.gaps == [(9, 2)] and run.alive["guardian"] == 0 and run.done
    assert not ck.trusted_alive(run).passed
    assert ck.contract_matches(run, SPEC).passed


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
