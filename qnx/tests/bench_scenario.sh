#!/bin/sh
# One fault-bench scenario on the target (run over SSH by `arbiter bench run`):
# start a fresh watchdog, arm and guardian with the given contract table, play
# the command script from stdin through arb-inject, record whether every
# trusted process is still alive, then stop them. Each scenario starts from a
# clean state (home position, NORMAL, no counted violations).
#
#   ./bench_scenario.sh <contract.bin> [gateway] < script
#
# With "gateway" (decision D6: the scenarios about the AI side), the commands
# come from the planner on the host through arb-gateway, and the script holds no
# command: arb-inject only records the arm's telemetry and the guardian's status,
# from before the gateway starts until after the host is done.
#
# Output: arb-inject's records, then ALIVE, SLOG and LOG lines. Exit: arb-inject's status.
cd "$(dirname "$0")" || exit 1
. ./arb_suite_lib.sh             # on the policy image each process starts as its type
CONTRACT="${1:?usage: bench_scenario.sh <contract.bin> [gateway]}"
VIA="${2:-inject}"
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
# The operator (user 800) may reset SAFE_STOP (F7): the guardian is told its user
# ID, and gets a copy of the console outside root's home, which 800 cannot enter.
OPERATOR_UID=800
cp ./arb-ctl /tmp/arb-ctl-bench && chmod 755 /tmp/arb-ctl-bench
$ARB_ON_GUARD ./arb-guardian "$CONTRACT" "$OPERATOR_UID" </dev/null >"$LOGDIR/guardian.log" 2>&1 &
GUARD=$!
sleep 1
# The evidence logger (docs/EVIDENCE_LOG.md): the bench verifies its chain and
# that every verdict arb-inject received is in it (FR-E1, FR-E4).
rm -f "$LOGDIR/evidence.log" "$LOGDIR/evidence.log.anchor"
$ARB_ON_LOGD ./arb-logd "$CONTRACT" "$LOGDIR/evidence.log" </dev/null >"$LOGDIR/logd.log" 2>&1 &
LOGD=$!
sleep 1

# F12: the CPU hog, where the untrusted user can run it (not in root's home).
cp ./arb-hog /tmp/arb-hog && chmod 755 /tmp/arb-hog
# Actions name processes by placeholder: fill in this run's pids (F13), and the
# number of CPUs for the hog's threads (F12; one "ProcessorN:" line per CPU).
NCPU=$(pidin info | grep -c '^Processor')
sed -e "s/@GUARD@/$(pid_of arb-guardian)/g" -e "s/@ARM@/$(pid_of arb-arm)/g" -e "s/@WD@/$(pid_of arb-watchdog)/g" \
    -e "s/@NCPU@/$NCPU/g" \
    "$LOGDIR/script.txt" >"$LOGDIR/script.run" && mv "$LOGDIR/script.run" "$LOGDIR/script.txt"

if [ "$VIA" = gateway ]; then
    ./arb-inject <"$LOGDIR/script.txt" >"$LOGDIR/inject.out" 2>&1 &
    INJ=$!
    sleep 0.2
    $ARB_ON_GW ./arb-gateway ${ARB_GW_ARGS:-} </dev/null >"$LOGDIR/gateway.log" 2>&1 &
    wait "$INJ"
    rc=$?
    cat "$LOGDIR/inject.out"
    slay -f arb-gateway >/dev/null 2>&1
else
    ./arb-inject <"$LOGDIR/script.txt"
    rc=$?
fi

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
slog logd arb_logd arb-logd "$LOGD"

# Stop the logger cleanly (its E line), then print its log and anchor.
LOGD_PID=$(pidin -p arb-logd -F "%a" 2>/dev/null | tail -1 | tr -d ' ')
case "$LOGD_PID" in ''|*[!0-9]*) LOGD_PID=$LOGD ;; esac
kill -TERM "$LOGD_PID" 2>/dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do       # bound: 10 x 0.1 s
    kill -0 "$LOGD_PID" 2>/dev/null || break
    sleep 0.1
done
wait "$LOGD" 2>/dev/null
sed 's/^/EVLOG /' "$LOGDIR/evidence.log" 2>/dev/null
[ -r "$LOGDIR/evidence.log.anchor" ] && echo "EVANCHOR $(cat "$LOGDIR/evidence.log.anchor")"

# F12 backstop: no hog outlives its scenario (arb-inject kills it too).
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/slay.html
slay -f arb-hog >/dev/null 2>&1
rm -f /tmp/arb-hog
kill "$GUARD" "$ARM" 2>/dev/null
wait "$GUARD" 2>/dev/null
wait "$ARM" 2>/dev/null
kill "$WD" 2>/dev/null
wait "$WD" 2>/dev/null
for p in watchdog arm guardian logd gateway; do
    [ -r "$LOGDIR/$p.log" ] && sed "s/^/LOG $p /" "$LOGDIR/$p.log"
done
exit "$rc"
