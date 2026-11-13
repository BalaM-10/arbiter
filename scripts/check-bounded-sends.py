#!/usr/bin/env python3
"""FR-G9 source check: every MsgSend() in the guardian is bounded by TimerTimeout().

TimerTimeout() applies only to "the next kernel call made by the same thread"
(QNX 8.0 TimerTimeout() page), so it must come immediately before MsgSend(),
with nothing in between except its own error branch. For every MsgSend( in
qnx/guardian/*.c, walk back over blank lines, comments, braces and the error
branch's `return`/assignment lines; the first real statement must call
TimerTimeout(.

Run:  python3 scripts/check-bounded-sends.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP = re.compile(r"^\s*(?:$|//|/\*|\*|\{|\}|return\b|l->last_errno\s*=)")


def fr_g9_timer_timeout_immediately_precedes_send(sources: list[Path]) -> list[str]:
    problems: list[str] = []
    sends = 0
    for path in sources:
        lines = path.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            if "MsgSend(" not in line or line.lstrip().startswith(("*", "/*", "//")):
                continue
            sends += 1
            j = i - 1
            while j >= 0 and SKIP.match(lines[j]):
                j -= 1
            if j < 0 or "TimerTimeout(" not in lines[j]:
                problems.append(f"{path.relative_to(ROOT)}:{i + 1}: MsgSend() not directly preceded by TimerTimeout()")
    if sends == 0:
        problems.append("no MsgSend() found in qnx/guardian: the check would prove nothing")
    return problems


def main() -> int:
    sources = sorted((ROOT / "qnx" / "guardian").glob("*.c"))
    problems = fr_g9_timer_timeout_immediately_precedes_send(sources)
    for p in problems:
        print(f"FAIL {p}")
    print(f"RESULT {'FAIL' if problems else 'PASS'} fr_g9_timer_timeout_immediately_precedes_send")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
