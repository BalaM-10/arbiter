"""Read a QNX kernel trace, as decoded by traceprinter, for F12 (NFR-Q5).

The bench records one with tracelogger on the target while F12 runs
(runner.py) and decodes it on the host with traceprinter. This module turns the
text into a summary of what the scheduler did while the CPU hog ran:

- the hog's threads: their process, priority and how often they ran;
- for each trusted process (arm, watchdog, guardian): how often its threads ran
  inside the hog's window, and the longest time one of them waited between
  becoming READY and RUNNING;
- how long, in total and at most at once, a hog thread was RUNNING on some CPU
  while a trusted thread was READY and not yet running;
- whether the trace is whole: the kernel numbers its event buffers per CPU
  (``CONTROL :BUFFER sequence = <n>``, observed in traceprinter's output, not
  described on its page); a gap in that sequence is lost events.

On QEMU every duration here proves function, never timing (owner decision
2026-11-13); the bench reports them and judges only that the trusted threads ran.
On QEMU they are also unverified: in the first F12 runs (2026-11-18) the trace
showed the guardian's thread READY at 64 for up to 35 ms on one CPU while the
watchdog, in the same run, saw no 10 ms gap in its heartbeats. Until that is
explained (on the Pi 5, M3), no claim rests on these durations.

traceprinter's format (QNX 8.0 traceprinter page): one event per line,
``t:<cycles> CPU:<nn> <CLASS> :<EVENT> <args>``; thread events carry
``pid:<n> tid:<n> priority:<n> policy:<n>``; PROCCREATE_NAME continues on the
following lines with ``pid:`` and ``name:``; TIME events carry the msb of the
cycle counter, and a ``t:`` field of 8 hex digits or fewer is its lsb only.
https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/t/traceprinter.html
"""

from __future__ import annotations

import re
from typing import Any

TRUSTED = ("arb-arm", "arb-watchdog", "arb-guardian")
HOG = "arb-hog"
HOG_PRIO = 63           # its spinning threads; its main thread only starts and joins them
PRIVILEGED = 64         # a trusted thread counts when it is at 64 or above (arb_prio.h); below
                        # that (e.g. at startup, before it raises its priority) it is not protected

_EV = re.compile(r"^t:0x([0-9a-fA-F]+)\s+CPU:(\d+)\s+(\S+)\s*:(\S+)\s*(.*)$")
_KV = re.compile(r"(\w+):(\S+)")
_HDR = re.compile(r"^\s*(TRACE_[A-Z_]+)::\s*(.*)$")
_SEQ = re.compile(r"sequence = (\d+)")


def _base(name: str) -> str:
    return name.rsplit("/", 1)[-1]


def pids_from(slog: list[str], hog_pid: int | None) -> dict[int, str]:
    """Which process is which, from the scenario's own records: each trusted process's
    system-log lines name its buffer set and pid (arb_arm.<pid>), and the hog prints
    its pid. A process started after the trace began keeps, in the trace, the name of
    the shell it was forked from, so the trace's own names cannot be used for them."""
    out: dict[int, str] = {}
    for line in slog:
        m = re.search(r"\barb_(arm|watchdog|guardian)\.(\d+)\b", line)
        if m:
            out[int(m.group(2))] = f"arb-{m.group(1)}"
    if hog_pid is not None and hog_pid > 0:
        out[hog_pid] = HOG
    return out


def summarize(text: str, pids: dict[int, str] | None = None) -> dict[str, Any]:
    """The summary described above; {"error": ...} if the text holds no usable trace.
    pids, if given, says which process is which (pids_from); else the trace's names."""
    header: dict[str, str] = {}
    names: dict[int, str] = {}
    msb = 0
    pending_proc: dict[str, str] | None = None
    events: list[tuple[int, int, str, dict[str, str]]] = []   # (t, cpu, event, args)
    last_seq: dict[int, int] = {}
    buffer_gaps = 0

    for line in text.splitlines():
        h = _HDR.match(line)
        if h:
            header[h.group(1)] = h.group(2).strip()
            continue
        m = _EV.match(line)
        if m is None:
            if pending_proc is not None:
                for k, v in _KV.findall(line):
                    pending_proc[k] = v
                if "name" in pending_proc and "pid" in pending_proc:
                    names[int(pending_proc["pid"])] = _base(pending_proc["name"])
            continue
        pending_proc = None
        raw, cpu, cls, ev, rest = m.group(1), int(m.group(2)), m.group(3), m.group(4), m.group(5)
        args = dict(_KV.findall(rest))
        if cls == "CONTROL" and ev == "TIME" and "msb" in args:
            msb = int(args["msb"], 16)
        if cls == "CONTROL" and ev == "BUFFER":
            q = _SEQ.search(rest)
            if q:
                n = int(q.group(1))
                if cpu in last_seq and n != last_seq[cpu] + 1:
                    buffer_gaps += max(0, n - last_seq[cpu] - 1)
                last_seq[cpu] = n
        t = int(raw, 16)
        if len(raw) <= 8:
            t = (msb << 32) | t
        if ev in ("PROCCREATE_NAME", "PROCTHREAD_NAME"):
            pending_proc = args if ev == "PROCCREATE_NAME" else None
            if pending_proc is not None and "name" in args and "pid" in args:
                names[int(args["pid"])] = _base(args["name"])
            continue
        if cls == "THREAD" and "pid" in args and "tid" in args:
            events.append((t, cpu, ev, args))

    cps = int(header.get("TRACE_CYCLES_PER_SEC", "0") or 0)
    if cps <= 0 or not events:
        return {"error": "no thread events or no TRACE_CYCLES_PER_SEC in the trace"}
    events.sort(key=lambda e: e[0])
    us = 1e6 / cps
    who = pids if pids else names
    hog_pids = {p for p, n in who.items() if n == HOG}
    trusted_pids = {p: n for p, n in who.items() if n in TRUSTED}

    hog_runs = [e for e in events if e[2] == "THRUNNING" and int(e[3]["pid"]) in hog_pids
                and int(e[3].get("priority", "-1")) == HOG_PRIO]
    if not hog_runs:
        return {"error": f"no {HOG} thread ran at {HOG_PRIO} in the trace",
                "processes": sorted(set(names.values()))}
    hog_threads = {(int(e[3]["pid"]), int(e[3]["tid"])) for e in hog_runs}
    hog_ev = [e for e in events if (int(e[3]["pid"]), int(e[3]["tid"])) in hog_threads]
    w0, w1 = hog_runs[0][0], hog_ev[-1][0]

    running: dict[int, tuple[int, int]] = {}                  # cpu -> (pid, tid)
    ready: dict[tuple[int, int], int] = {}                    # trusted (pid, tid) -> READY since t
    per: dict[str, dict[str, Any]] = {n: {"runs": 0, "max_ready_us": 0.0, "waits": 0} for n in TRUSTED}
    overlap_total = overlap_cur = overlap_max = 0
    last_t = w0
    for t, cpu, ev, a in events:
        if t < w0 or t > w1:
            continue
        hog_on_cpu = any(k in hog_threads for k in running.values())
        if ready and hog_on_cpu:
            overlap_total += t - last_t
            overlap_cur += t - last_t
            overlap_max = max(overlap_max, overlap_cur)
        else:
            overlap_cur = 0
        last_t = t
        key = (int(a["pid"]), int(a["tid"]))
        prio = int(a.get("priority", "-1"))
        if ev == "THRUNNING":
            running[cpu] = key
            if key[0] in trusted_pids and prio >= PRIVILEGED:
                p = per[trusted_pids[key[0]]]
                p["runs"] += 1
                if key in ready:
                    p["max_ready_us"] = max(p["max_ready_us"], (t - ready.pop(key)) * us)
                    p["waits"] += 1
        elif ev == "THREADY":
            if key[0] in trusted_pids and prio >= PRIVILEGED:
                ready.setdefault(key, t)
            for c, k in list(running.items()):
                if k == key:
                    del running[c]
        else:                                                 # blocked, stopped, dead: off the CPU
            ready.pop(key, None)
            for c, k in list(running.items()):
                if k == key:
                    del running[c]

    prios = sorted({int(e[3].get("priority", "-1")) for e in hog_runs})
    return {
        "cpus": int(header.get("TRACE_CPU_NUM", "0") or 0),
        "cycles_per_sec": cps,
        "thread_events": len(events),
        "buffer_gaps": buffer_gaps,
        "hog": {"pids": sorted(hog_pids), "threads": len(hog_threads), "priorities": prios,
                "runs": len(hog_runs), "window_ms": round((w1 - w0) * us / 1000, 3)},
        "trusted": {n: {"runs": v["runs"], "waits": v["waits"], "max_ready_us": round(v["max_ready_us"], 1)}
                    for n, v in per.items()},
        "hog_running_while_trusted_ready_us": {"total": round(overlap_total * us, 1),
                                               "longest": round(overlap_max * us, 1)},
    }
