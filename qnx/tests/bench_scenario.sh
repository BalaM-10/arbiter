#!/bin/sh
# One fault-bench scenario on the target (run over SSH by `arbiter bench run`):
# start a fresh watchdog, arm and guardian with the given contract table, play
# the command script from stdin through arb-inject, record whether every
# trusted process is still alive, then stop them. Each scenario starts from a
# clean state (home position, NORMAL, no counted violations).
#
#   ./bench_scenario.sh <contract.bin> < script
#
# Output: arb-inject's records, then ALIVE and LOG lines. Exit: arb-inject's status.
cd "$(dirname "$0")" || exit 1
CONTRACT="${1:?usage: bench_scenario.sh <contract.bin>}"
LOGDIR=/tmp/arbiter-bench
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/*.log
cat >"$LOGDIR/script.txt"     # read the script before anything else can touch stdin

./arb-watchdog </dev/null >"$LOGDIR/watchdog.log" 2>&1 &
WD=$!
sleep 1
./arb-arm "$CONTRACT" </dev/null >"$LOGDIR/arm.log" 2>&1 &
ARM=$!
sleep 1
./arb-guardian "$CONTRACT" </dev/null >"$LOGDIR/guardian.log" 2>&1 &
GUARD=$!
sleep 1

./arb-inject <"$LOGDIR/script.txt"
rc=$?

alive() { kill -0 "$1" 2>/dev/null && echo 1 || echo 0; }
echo "ALIVE watchdog $(alive "$WD") arm $(alive "$ARM") guardian $(alive "$GUARD")"

kill "$GUARD" "$ARM" 2>/dev/null
wait "$GUARD" 2>/dev/null
wait "$ARM" 2>/dev/null
kill "$WD" 2>/dev/null
wait "$WD" 2>/dev/null
for p in watchdog arm guardian; do
    sed "s/^/LOG $p /" "$LOGDIR/$p.log"
done
exit "$rc"
