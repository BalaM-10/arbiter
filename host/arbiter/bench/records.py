"""Parse arb-inject's output (format documented in qnx/tests/arb_inject.c)."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Verdict:
    t_send_ns: int
    t_reply_ns: int
    telem_head: int
    seq: int
    rc: int
    verdict: int
    rule: int
    state: int
    flags: int
    target: tuple[int, int]
    duration_ms: int
    requested_ms: int


@dataclass(frozen=True)
class Sample:
    index: int
    t_ns: int
    t_cycles: int
    pos: tuple[int, int]
    vel: tuple[int, int]
    segment: int
    state: int
    lease_ok: int


@dataclass(frozen=True)
class Status:
    t_ns: int
    state: int
    stop_cause: int
    decisions: int
    arm_faults: int
    ticks: int
    bad_messages: int
    watchdog_trips: int
    foreign_pulses: int
    watchdog_lost: int = 0      # times the guardian lost the watchdog itself (FR-W5, FR-W6)
    watchdog_lost_cause: int = 0  # ARB_WD_LOST_* of that first loss (1 connection died, 2 no beat)


@dataclass
class Run:
    verdicts: list[Verdict] = field(default_factory=list)
    samples: list[Sample] = field(default_factory=list)
    statuses: list[Status] = field(default_factory=list)
    gaps: list[tuple[int, int]] = field(default_factory=list)
    counters: dict[str, dict[str, int]] = field(default_factory=dict)      # the arm's, by phase
    wd: dict[str, dict[str, int]] = field(default_factory=dict)            # the watchdog's, by phase
    alive: dict[str, int] = field(default_factory=dict)
    logs: list[str] = field(default_factory=list)
    slog: list[str] = field(default_factory=list)          # "<process> <slog2info line>", this scenario only
    evlog: list[str] = field(default_factory=list)         # arb-logd's log lines (docs/EVIDENCE_LOG.md)
    evanchor: str = ""                                     # its published last hash
    done: bool = False
    exit_code: int = -1
    t0_ns: int = 0          # clock of the first command; scenario times are relative to it
    target: str = ""        # target name from scripts/targets.env (decides whether timing is judged)


def parse(text: str, exit_code: int) -> Run:
    run = Run(exit_code=exit_code)
    for line in text.splitlines():
        f = line.split()
        if not f:
            continue
        tag = f[0]
        if tag == "V" and len(f) == 14:
            n = [int(x) for x in f[1:]]
            run.verdicts.append(Verdict(n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8],
                                        (n[9], n[10]), n[11], n[12]))
        elif tag == "T" and len(f) == 11:
            n = [int(x) for x in f[1:]]
            run.samples.append(Sample(n[0], n[1], n[2], (n[3], n[4]), (n[5], n[6]), n[7], n[8], n[9]))
        elif tag == "S" and len(f) in (11, 12):
            n = [int(x) for x in f[1:]]
            run.statuses.append(Status(*n))
        elif tag == "WD" and len(f) >= 14 and len(f) % 2 == 0:
            run.wd[f[1]] = {f[i]: int(f[i + 1]) for i in range(2, len(f), 2)}
        elif tag == "GAP" and len(f) == 3:
            run.gaps.append((int(f[1]), int(f[2])))
        elif tag == "COUNTERS" and len(f) >= 14 and len(f) % 2 == 0:
            run.counters[f[1]] = {f[i]: int(f[i + 1]) for i in range(2, len(f), 2)}
        elif tag == "ALIVE" and len(f) == 7:
            run.alive = {f[i]: int(f[i + 1]) for i in range(1, 7, 2)}
        elif tag == "EVLOG":
            run.evlog.append(line[len("EVLOG "):])
        elif tag == "EVANCHOR" and len(f) == 2:
            run.evanchor = f[1]
        elif tag == "SLOG":
            run.slog.append(line[len("SLOG "):])
        elif tag == "LOG":
            run.logs.append(" ".join(f[1:]))
        elif tag == "DONE":
            run.done = True
    if run.verdicts:
        run.t0_ns = run.verdicts[0].t_send_ns
    return run
