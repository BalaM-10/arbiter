"""F12 CPU hog: an untrusted busy loop at the highest unprivileged priority, on every CPU."""

from __future__ import annotations

from itertools import pairwise
from typing import Any

from .. import checks as ck
from ..checks import Check
from ..records import Run
from ..script import Script
from ..spec import Spec
from .common import baseline

ID = "F12"
TRANSPORT = "inject"    # decision D6: an untrusted process on the target, so a tool on the target
TRACE = True            # NFR-Q5: the runner records a kernel trace while this scenario runs
FALLS_SILENT = True     # nothing sent during the hog: the planner stand-in (10) would be starved anyway
TITLE = "CPU hog"
HOG_S = 3               # fixed duration; the hog's hard cap is 30 s (owner decision 2026-11-17)
HOG_PRIO = 63           # ARB_PRIO_UNPRIVILEGED_MAX: the most a process without PROCMGR_AID_PRIORITY may use
UNTRUSTED_UID = 820     # ARB_UID_UNTRUSTED
PASS_CONDITION = (
    f"While the arm moves, an untrusted process (user {UNTRUSTED_UID}) busy-loops for {HOG_S} s at "
    f"priority {HOG_PRIO} FIFO, the highest an unprivileged process may use, on every CPU. The trusted "
    "processes, all at 64 and above, keep running: the arm's loop never stops (tick spacing judged on the "
    "Pi 5, measured and reported on QEMU), the watchdog keeps accepting the guardian's and the arm's "
    "heartbeats, nothing trips, faults or brakes, and no limit, keep-out or speed rule is broken. The "
    "planner stand-in, itself untrusted at priority 10, sends nothing during the hog (it would be starved), "
    "so the guardian holds; after the hog ends, the next command is allowed and the arm reaches it, with "
    "no operator reset. A kernel trace of the run is saved."
)

A = (-400_000, 200_000)
HOG_AT_MS = 300
BACK_AT_MS = HOG_AT_MS + HOG_S * 1000 + 500


def build(spec: Spec) -> Script:
    s = Script()
    s.send(A, 2000, "move", gap_ms=HOG_AT_MS)                 # moving when the hog starts
    s.action("hog", f"{HOG_S},@NCPU@")
    s.t_ms = BACK_AT_MS
    s.status()
    # After the hog: back home, re-sent every 150 ms with the time left, so the
    # arrival time stays put and the guardian hears from the planner (as F10 does).
    for k in range(10):
        s.marks["home"] = len(s.commands)
        s.send((0, 0), 1500 - 150 * k, "home")
    s.wait(900)
    s.status()
    s.end(after_ms=0)
    return s


def hog_ran(run: Run) -> Check:
    """The hog really ran as the untrusted user, at 63 FIFO, on every thread it was given, for its time."""
    h: dict[str, Any] = run.hog
    act = next((a for a in run.actions if a.name == "hog"), None)
    threads = h.get("thread", [])
    want = int(act.arg.split(",")[1]) if act is not None and "," in act.arg else -1
    ok = (act is not None and act.rc == 0 and h.get("uid") == UNTRUSTED_UID and h.get("euid") == UNTRUSTED_UID
          and h.get("ok") == 1 and len(threads) == want and want >= 1
          and all(t.get("prio") == HOG_PRIO and t.get("policy") == 1 and t.get("loops", 0) > 0
                  and t.get("ran_ms", 0) >= 0.9 * HOG_S * 1000 for t in threads))
    started = f"rc {act.rc}" if act else "no hog action"
    detail = (f"started: {started}; user {h.get('uid')}/{h.get('euid')}; {len(threads)} of {want} threads; " +
              ", ".join(f"thread {t.get('i')} prio {t.get('prio')} policy {t.get('policy')} "
                        f"ran {t.get('ran_ms')} ms" for t in threads))
    return Check("hog_ran_as_untrusted_at_63_on_every_cpu", ok, detail, {"hog": h})


def arm_loop_never_stopped(run: Run) -> Check:
    """Inside the hog's time the arm's samples keep coming. A starved arm loop would
    leave a gap as long as the hog; the 2-period rule itself is no_deadline_miss's."""
    act = next((a for a in run.actions if a.name == "hog"), None)
    if act is None:
        return Check("arm_loop_ran_through_the_hog", False, "no hog action")
    lo, hi = act.t_ns, act.t_ns + HOG_S * 1_000_000_000
    win = [s for s in run.samples if lo <= s.t_ns <= hi]
    gaps = [b.t_ns - a.t_ns for a, b in pairwise(win)]
    worst = max(gaps, default=0) / 1e6
    ok = len(win) >= 2 and worst < 100.0
    return Check("arm_loop_ran_through_the_hog", ok,
                 f"{len(win)} arm samples in the hog's {HOG_S} s, longest interval {worst:.3f} ms "
                 f"(function: under 100 ms; timing: see no_deadline_miss)",
                 {"samples": len(win), "longest_interval_ms": worst})


def kernel_trace(run: Run) -> Check:
    """NFR-Q5: the trace was recorded, the hog's threads appear in it at 63, and every
    trusted process ran inside the hog's window. Durations are reported, not judged."""
    t = run.trace
    if not t or "error" in t:
        return Check("kernel_trace_shows_the_trusted_processes_running", False,
                     f"no usable trace: {t.get('error') if t else 'not recorded'}", {"trace": t})
    hog = t["hog"]
    tr = t["trusted"]
    whole = t.get("buffer_gaps", -1) == 0
    ok = (whole and hog["priorities"] == [HOG_PRIO] and hog["runs"] > 0 and hog["threads"] >= 1
          and all(v["runs"] > 0 for v in tr.values()))
    ov = t["hog_running_while_trusted_ready_us"]
    detail = (f"{t['thread_events']} thread events on {t['cpus']} CPUs, buffer gaps {t.get('buffer_gaps')}; "
              f"hog: {hog['threads']} threads at "
              f"{hog['priorities']}, window {hog['window_ms']} ms; " +
              "; ".join(f"{n}: ran {v['runs']} times, longest READY to RUNNING {v['max_ready_us']} us"
                        for n, v in tr.items()) +
              f"; a hog thread RUNNING while a trusted thread was READY: {ov['total']} us in all, "
              f"longest {ov['longest']} us (durations from the trace's timestamps: reported, not judged; "
              "on QEMU unverified, see trace.py)")
    return Check("kernel_trace_shows_the_trusted_processes_running", ok, detail, {"trace": t})


def f12_cpu_hog_trusted_processes_keep_running(run: Run, script: Script, spec: Spec) -> list[Check]:
    out = baseline(run, script, spec)            # includes trusted_processes_running: the heartbeats
    out.append(hog_ran(run))
    out.append(arm_loop_never_stopped(run))
    out.append(ck.no_missed_cycle(run, script))
    out.append(ck.never_safe_stop(run))
    out.append(ck.verdicts_as_expected(run, script, {"move": ("ALLOW", "NONE"), "home": ("ALLOW", "NONE")}))
    out.append(ck.reached(run, script.commands[script.marks["home"]], "reached_home_after_the_hog"))
    out.append(kernel_trace(run))
    return out


judge = f12_cpu_hog_trusted_processes_keep_running
