#!/usr/bin/env python3
"""NFR-Q2 source check: the trusted code talks only through QNX native IPC.

NFR-Q2: command and setpoint paths use QNX synchronous message passing;
heartbeats and timer ticks use pulses. Over qnx/{guardian,arm,watchdog,common}
this check requires:

  1. no other IPC: no socket, pipe, FIFO, POSIX or System V message queue call;
  2. the command path is the guardian's resource manager (resmgr_attach on
     /dev/arbiter, with a devctl handler) and the setpoint path is MsgSend()
     in qnx/guardian/arm_link.c (synchronous: the guardian waits for the reply);
  3. every timer_create() is given an event built with SIGEV_PULSE_INIT, so
     every timer tick is a pulse;
  4. heartbeats and the guardian-to-arm lease/hold/brake signals are
     MsgSendPulse() calls, and the watchdog's trip events are pulses delivered
     with MsgDeliverEvent().

Each rule also demands positive evidence (the call exists), so the check cannot
pass by finding nothing. The run-time half is on target:
qnx/tests/test_e2e.c nfr_q2_* (run by arm_suite.sh).

Run:  python3 scripts/check-ipc.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRUSTED_DIRS = ("guardian", "arm", "watchdog", "common")

FORBIDDEN = re.compile(
    r"\b(socket|socketpair|bind|listen|accept|connect|send|recv|sendto|recvfrom|sendmsg|recvmsg|"
    r"pipe|pipe2|mkfifo|popen|mq_open|mq_send|mq_receive|msgget|msgsnd|msgrcv)\s*\(")
TIMER = re.compile(r"timer_create\s*\(\s*\w+\s*,\s*&(\w+)")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def _code(path: Path) -> str:
    """Source text with comments removed, so prose cannot satisfy or fail a rule."""
    return COMMENT.sub(" ", path.read_text(encoding="utf-8"))


def _function_body(code: str, name: str) -> str:
    m = re.search(rf"\b{name}\s*\([^;{{]*\)\s*\{{", code)
    if not m:
        return ""
    depth, i = 1, m.end()
    while i < len(code) and depth:          # bounded by the file length
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.end():i]


def nfr_q2_trusted_code_uses_only_native_ipc(sources: list[Path]) -> list[str]:
    problems: list[str] = []
    code = {p: _code(p) for p in sources}
    rel = {p: str(p.relative_to(ROOT)) for p in sources}
    by_name = {rel[p]: c for p, c in code.items()}

    # 1. No other IPC anywhere in trusted code.
    for p, c in code.items():
        for m in FORBIDDEN.finditer(c):
            line = c.count("\n", 0, m.start()) + 1
            problems.append(f"{rel[p]}:{line}: {m.group(1)}() is not QNX native IPC")

    # 2. Command path: resource manager with a devctl handler. Setpoint path: MsgSend().
    g = by_name.get("qnx/guardian/main.c", "")
    if "resmgr_attach(" not in g or "ARB_DEV_PATH" not in g or "io_devctl" not in g:
        problems.append("qnx/guardian/main.c: no resmgr_attach() on ARB_DEV_PATH with a devctl handler")
    link = by_name.get("qnx/guardian/arm_link.c", "")
    if "MsgSend(" not in link:
        problems.append("qnx/guardian/arm_link.c: the setpoint path has no MsgSend()")

    # 3. Every timer delivers a pulse.
    timers = 0
    for p, c in code.items():
        for m in TIMER.finditer(c):
            timers += 1
            ev = m.group(1)
            if not re.search(rf"SIGEV_PULSE_INIT\s*\(\s*&{ev}\b", c[:m.start()]):
                line = c.count("\n", 0, m.start()) + 1
                problems.append(f"{rel[p]}:{line}: timer_create() event '{ev}' is not built with SIGEV_PULSE_INIT")
    if timers < 3:
        problems.append(f"found {timers} timer_create() calls, expected one each in guardian, arm and watchdog")
    for p, c in code.items():
        if re.search(r"\bSIGEV_(SIGNAL|THREAD)\w*", c):
            problems.append(f"{rel[p]}: a signal or thread event is used; ticks and events must be pulses")

    # 4. Heartbeats and guardian-to-arm signals are pulses; watchdog trips are delivered events.
    hb = _function_body(by_name.get("qnx/common/arb_wd_client.c", ""), "arb_wd_client_heartbeat")
    if "MsgSendPulse(" not in hb or "MsgSend(" in hb:
        problems.append("qnx/common/arb_wd_client.c: arb_wd_client_heartbeat() is not a single MsgSendPulse()")
    pulse = _function_body(g, "arm_pulse")
    if "MsgSendPulse(" not in pulse or "MsgSend(" in pulse:
        problems.append("qnx/guardian/main.c: arm_pulse() (lease, hold, brake) is not a MsgSendPulse()")
    if "MsgDeliverEvent(" not in by_name.get("qnx/watchdog/main.c", ""):
        problems.append("qnx/watchdog/main.c: trips are not delivered with MsgDeliverEvent()")
    return problems


def main() -> int:
    sources = sorted(p for d in TRUSTED_DIRS for pat in ("*.c", "*.h") for p in (ROOT / "qnx" / d).glob(pat))
    problems = nfr_q2_trusted_code_uses_only_native_ipc(sources)
    for p in problems:
        print(f"FAIL {p}")
    print(f"RESULT {'FAIL' if problems else 'PASS'} nfr_q2_trusted_code_uses_only_native_ipc "
          f"({len(sources)} trusted source files)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
