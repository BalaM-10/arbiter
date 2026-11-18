"""F12 (CPU hog) and NFR-Q5 (kernel trace): the bench's own checks, on made-up records.

Each check must fail on the record that shows the fault it is there to catch,
and pass on one that does not."""

from __future__ import annotations

from arbiter.bench import trace as ktrace
from arbiter.bench.records import Action, Run, Sample, parse
from arbiter.bench.scenarios import f12

MS = 1_000_000

GOOD_HOG = """HOG start uid 820 euid 820 threads 2 seconds 3 pid 500
HOG thread 0 prio 63 policy 1 loops 2500000000 ran_ms 3000
HOG thread 1 prio 63 policy 1 loops 2490000000 ran_ms 2999
HOG end ok 1
A 1000000000 hog 3,2 0
"""


def _hog_run(text: str = GOOD_HOG) -> Run:
    return parse(text, 0)


def f12_hog_check_passes_on_a_hog_at_63_on_every_cpu() -> None:
    c = f12.hog_ran(_hog_run())
    assert c.passed, c.detail


def f12_hog_check_fails_below_63_as_root_or_on_too_few_threads() -> None:
    assert not f12.hog_ran(_hog_run(GOOD_HOG.replace("prio 63 policy 1 loops 2490000000",
                                                      "prio 10 policy 1 loops 2490000000"))).passed
    assert not f12.hog_ran(_hog_run(GOOD_HOG.replace("uid 820 euid 820", "uid 0 euid 0"))).passed
    act = "A 1000000000 hog 3,2 0"
    assert not f12.hog_ran(_hog_run(GOOD_HOG.replace(act, "A 1000000000 hog 3,4 0"))).passed   # 2 of 4
    assert not f12.hog_ran(_hog_run(GOOD_HOG.replace("ran_ms 2999", "ran_ms 120"))).passed
    assert not f12.hog_ran(_hog_run(GOOD_HOG.replace(act, "A 1000000000 hog 3,2 13"))).passed  # not started
    assert not f12.hog_ran(parse("", 0)).passed


def _arm_run(gap_from_ms: int | None, gap_ms: int) -> Run:
    run = Run()
    run.actions.append(Action(1000 * MS, "hog", "3,2", 0))
    t, i = 0, 0
    while t <= 5000 * MS:
        run.samples.append(Sample(i, t, i, (0, 0), (0, 0), 1, 0, 1))
        i += 1
        t += MS
        if gap_from_ms is not None and t == gap_from_ms * MS:
            t += gap_ms * MS
    return run


def f12_arm_loop_check_fails_on_a_starved_arm() -> None:
    assert f12.arm_loop_never_stopped(_arm_run(None, 0)).passed
    assert f12.arm_loop_never_stopped(_arm_run(2000, 5)).passed            # a late tick: not this check's
    starved = f12.arm_loop_never_stopped(_arm_run(1100, 2800))
    assert not starved.passed and "2801" in starved.detail, starved.detail


# A traceprinter excerpt in the documented format: the hog's two threads at 63 on
# both CPUs; the arm (70) and the watchdog (66) becoming READY and running; the
# guardian (64) running once.
TRACE = """TRACEPRINTER version 1.02
 -- HEADER FILE INFORMATION --
  TRACE_CYCLES_PER_SEC:: 1000000000
         TRACE_CPU_NUM:: 2
 -- KERNEL EVENTS --
t:0x00000010 CPU:00 CONTROL         :TIME               msb:0x00000000 lsb(offset):0x00000010
t:0x00000010 CPU:00 CONTROL         :BUFFER             sequence = 1, num_events = 30
t:0x00000010 CPU:01 CONTROL         :BUFFER             sequence = 1, num_events = 12
t:0x00000011 CPU:00 PROCESS         :PROCCREATE_NAME
                      ppid:1
                       pid:500
                      name:/tmp/arb-hog
t:0x00000012 CPU:00 PROCESS         :PROCCREATE_NAME
                      ppid:1
                       pid:300
                      name:./arb-arm
t:0x00000013 CPU:00 PROCESS         :PROCCREATE_NAME
                      ppid:1
                       pid:301
                      name:./arb-watchdog
t:0x00000014 CPU:00 PROCESS         :PROCCREATE_NAME
                      ppid:1
                       pid:302
                      name:./arb-guardian
t:0x00000900 CPU:00 THREAD          :THRUNNING          pid:500 tid:1 priority:10 policy:2
t:0x00001000 CPU:00 THREAD          :THRUNNING          pid:500 tid:2 priority:63 policy:1
t:0x00001000 CPU:01 THREAD          :THRUNNING          pid:500 tid:3 priority:63 policy:1
t:0x00002000 CPU:00 THREAD          :THREADY            pid:300 tid:1 priority:70 policy:1
t:0x00002800 CPU:00 THREAD          :THREADY            pid:500 tid:2 priority:63 policy:1
t:0x00002800 CPU:00 THREAD          :THRUNNING          pid:300 tid:1 priority:70 policy:1
t:0x00003000 CPU:00 THREAD          :THRECEIVE          pid:300 tid:1 priority:70 policy:1
t:0x00003000 CPU:00 THREAD          :THRUNNING          pid:500 tid:2 priority:63 policy:1
t:0x00003500 CPU:00 CONTROL         :BUFFER             sequence = 2, num_events = 30
t:0x00004000 CPU:01 THREAD          :THREADY            pid:301 tid:1 priority:66 policy:1
t:0x00004000 CPU:01 THREAD          :THREADY            pid:500 tid:3 priority:63 policy:1
t:0x00004000 CPU:01 THREAD          :THRUNNING          pid:301 tid:1 priority:66 policy:1
t:0x00004100 CPU:01 THREAD          :THRECEIVE          pid:301 tid:1 priority:66 policy:1
t:0x00004100 CPU:01 THREAD          :THRUNNING          pid:302 tid:1 priority:64 policy:1
t:0x00004200 CPU:01 THREAD          :THRECEIVE          pid:302 tid:1 priority:64 policy:1
t:0x00004200 CPU:01 THREAD          :THRUNNING          pid:500 tid:3 priority:63 policy:1
t:0x00009000 CPU:00 THREAD          :THDEAD             pid:500 tid:2 priority:63 policy:1
t:0x00009000 CPU:01 THREAD          :THDEAD             pid:500 tid:3 priority:63 policy:1
"""


def nfr_q5_trace_summary_reads_traceprinter_output() -> None:
    s = ktrace.summarize(TRACE)
    assert "error" not in s, s
    # the hog's main thread (at 10) is not one of its spinning threads
    assert s["cpus"] == 2 and s["hog"]["threads"] == 2 and s["hog"]["priorities"] == [63]
    assert s["buffer_gaps"] == 0
    assert s["trusted"]["arb-arm"]["runs"] == 1
    assert s["trusted"]["arb-arm"]["max_ready_us"] == 2.0                 # READY 0x2000, RUNNING 0x2800
    assert s["trusted"]["arb-watchdog"]["runs"] == 1 and s["trusted"]["arb-watchdog"]["max_ready_us"] == 0.0
    assert s["trusted"]["arb-guardian"]["runs"] == 1
    # The arm was READY from 0x2000 to 0x2800 while a hog thread ran on CPU 1: 2 us.
    assert s["hog_running_while_trusted_ready_us"]["total"] == 2.0


def nfr_q5_trace_summary_takes_the_pids_from_the_scenarios_records() -> None:
    """Processes started after the trace began carry their parent shell's name in it:
    which is which comes from the system log's buffer names and the hog's own line."""
    unnamed = TRACE.replace("name:/tmp/arb-hog", "name:ksh").replace("name:./arb-arm", "name:ksh")
    assert "error" in ktrace.summarize(unnamed)
    slog = ["arm Nov 18 00:31:43.072  arb_arm.300  main  1  ready as arbiter_arm",
            "watchdog Nov 18 00:31:43.072  arb_watchdog.301  main  1  ready",
            "guardian Nov 18 00:31:43.072  arb_guardian.302  main  1  ready on /dev/arbiter"]
    s = ktrace.summarize(unnamed, ktrace.pids_from(slog, 500))
    assert "error" not in s and s["trusted"]["arb-arm"]["runs"] == 1 and s["hog"]["threads"] == 2


def nfr_q5_trace_summary_counts_lost_event_buffers() -> None:
    s = ktrace.summarize(TRACE.replace("sequence = 2, num_events = 30", "sequence = 5, num_events = 30"))
    assert s["buffer_gaps"] == 3
    run = Run()
    run.trace = s
    assert not f12.kernel_trace(run).passed                               # an incomplete trace proves nothing


def nfr_q5_trace_summary_reports_a_trace_without_the_hog() -> None:
    s = ktrace.summarize(TRACE.replace("name:/tmp/arb-hog", "name:/tmp/other"))
    assert "error" in s
    assert "error" in ktrace.summarize("")


def f12_trace_check_needs_the_hog_at_63_and_every_trusted_process_running() -> None:
    run = Run()
    run.trace = ktrace.summarize(TRACE)
    assert f12.kernel_trace(run).passed
    run.trace = ktrace.summarize(TRACE.replace("pid:302 tid:1 priority:64 policy:1\n", "pid:999 tid:1 "
                                               "priority:64 policy:1\n"))
    assert not f12.kernel_trace(run).passed                               # the guardian never ran
    run.trace = {"error": "not recorded"}
    assert not f12.kernel_trace(run).passed
