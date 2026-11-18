#!/bin/bash
# The demo in one command (review M1-J13): on a running target, the scripted
# planner drives the arm through the gateway, then six fault scenarios show what
# the guardian, the arm and the watchdog do when something goes wrong. Nothing
# here is staged: it is the same planner end-to-end run and the same bench as the
# evidence runs, with a shorter scenario list. What each step shows: README,
# "What you will see".
#   ./scripts/demo.sh [target]          default: qemu
# Before: source ~/qnx800/qnxsdp-env.sh, and the target booted (README quick start).
# Output goes to build/demo/ (not evidence/): a demo run is not evidence.
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET="${1:-qemu}"
SCENARIOS="F1,F5,F10,F12,F13,F14"
OUT=build/demo
ARB=.venv/bin/arbiter
[ -x "$ARB" ] || { echo "demo: .venv not set up (docs/TARGETS.md, first-time setup)" >&2; exit 1; }
command -v qcc >/dev/null || { echo "demo: source ~/qnx800/qnxsdp-env.sh first" >&2; exit 1; }
command -v traceprinter >/dev/null || { echo "demo: traceprinter not on PATH (source ~/qnx800/qnxsdp-env.sh)" >&2; exit 1; }
mkdir -p "$OUT"

say() { printf '\n=== %s\n' "$*"; }

say "1/2  The AI proposes: the scripted planner sends the pick-and-place path over TCP to the gateway on QNX; the guardian decides every command"
"$ARB" planner e2e --target "$TARGET" --out "$OUT/runs"

say "2/2  QNX decides: $SCENARIOS (normal run, keep-out, AI killed, CPU hog, guardian killed, emergency stop)"
set +e
"$ARB" bench run --target "$TARGET" --scenarios "$SCENARIOS" --no-deploy --out "$OUT/bench" | tee "$OUT/bench.out"
rc=${PIPESTATUS[0]}
set -e
report="$(sed -n 's/^report: //p' "$OUT/bench.out" | tail -1)"

say "Done"
[ -n "$report" ] && echo "Open the report: $report"
exit "$rc"
