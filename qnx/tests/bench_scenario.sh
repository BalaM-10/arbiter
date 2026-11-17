#!/bin/sh
# One fault-bench scenario on the target (run over SSH by `arbiter bench run`):
# start a fresh watchdog, arm and guardian with the given contract table, play
# the command script from stdin through arb-inject, record whether every
# trusted process is still alive, then stop them. Each scenario starts from a
# clean state (home position, NORMAL, no counted violations).
#
#   ./bench_scenario.sh <contract.bin> < script
#
# Output: arb-inject's records, then ALIVE, SLOG and LOG lines. Exit: arb-inject's status.
cd "$(dirname "$0")" || exit 1
. ./arb_suite_lib.sh             # on the policy image each process starts as its type
CONTRACT="${1:?usage: bench_scenario.sh <contract.bin>}"
LOGDIR=/tmp/arbiter-bench
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/*.log
cat >"$LOGDIR/script.txt"     # read the script before anything else can touch stdin

$ARB_ON_WD ./arb-watchdog </dev/null >"$LOGDIR/watchdog.log" 2>&1 &
WD=$!
sleep 1
$ARB_ON_ARM ./arb-arm "$CONTRACT" </dev/null >"$LOGDIR/arm.log" 2>&1 &
ARM=$!
sleep 1
$ARB_ON_GUARD ./arb-guardian "$CONTRACT" </dev/null >"$LOGDIR/guardian.log" 2>&1 &
GUARD=$!
sleep 1

./arb-inject <"$LOGDIR/script.txt"
rc=$?

alive() { kill -0 "$1" 2>/dev/null && echo 1 || echo 0; }
echo "ALIVE watchdog $(alive "$WD") arm $(alive "$ARM") guardian $(alive "$GUARD")"

# This scenario's system log, read before the processes stop (slogger2 may drop
# a dead process's buffer later), one "SLOG <process> <line>" per line. The
# bench saves them per scenario, so a failed run names its cause.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/slog2info.html
# The pid is looked up by program name (on the policy image $! may be the pid
# of `on`), falling back to $! if the process is gone.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/p/pidin.html
slog() {    # <process> <buffer set> <program> <pid from $!>
    pid=$(pidin -p "$3" -F "%a" 2>/dev/null | tail -1 | tr -d ' ')
    case "$pid" in ''|*[!0-9]*) pid=$4 ;; esac
    if out=$(slog2info -b "$2" 2>&1); then
        printf '%s\n' "$out" | grep "$2\.$pid " | sed "s/^/SLOG $1 /"
    else
        echo "SLOG $1 unavailable: $(printf '%s' "$out" | head -1)"
    fi
}
slog watchdog arb_watchdog arb-watchdog "$WD"
slog arm arb_arm arb-arm "$ARM"
slog guardian arb_guardian arb-guardian "$GUARD"

kill "$GUARD" "$ARM" 2>/dev/null
wait "$GUARD" 2>/dev/null
wait "$ARM" 2>/dev/null
kill "$WD" 2>/dev/null
wait "$WD" 2>/dev/null
for p in watchdog arm guardian; do
    sed "s/^/LOG $p /" "$LOGDIR/$p.log"
done
exit "$rc"
