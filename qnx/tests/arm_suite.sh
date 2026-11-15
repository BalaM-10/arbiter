#!/bin/sh
# On-target arm controller tests (run from the deploy directory by scripts/run-tests.sh):
# the arm alone with test_arm playing the guardian, then the real guardian and the
# real arm end to end. Exit status 0 only if every case passed.
cd "$(dirname "$0")" || exit 1
CONTRACT=contract-corpus/contract.bin
fail=0
ARM=
GUARD=
WD=

arb_wd_up() {      # the watchdog starts first: arm and guardian register with it
    ./arb-watchdog &
    WD=$!
    sleep 1
}

arb_arm_up() {
    ./arb-arm "$CONTRACT" &
    ARM=$!
    sleep 1
}

arb_guard_up() {
    ./arb-guardian "$CONTRACT" &
    GUARD=$!
    sleep 1
}

arb_down() {
    kill -CONT "$WD" 2>/dev/null
    kill "$GUARD" "$ARM" 2>/dev/null
    kill -CONT "$GUARD" 2>/dev/null
    wait "$GUARD" 2>/dev/null
    wait "$ARM" 2>/dev/null
    kill "$WD" 2>/dev/null
    wait "$WD" 2>/dev/null
    GUARD=
    ARM=
    WD=
    sleep 1
}

echo "## FR-C4: the arm refuses to start on a bad contract table"
./arb-arm contract-corpus/crc.bin
rc=$?
if [ "$rc" -eq 2 ]; then
    echo "PASS fr_c4_arm_refuses_corrupted_contract (exit $rc)"
else
    echo "FAIL fr_c4_arm_refuses_corrupted_contract (exit $rc)"
    fail=1
fi

echo "## FR-W1: the arm refuses to start without the watchdog"
./arb-arm "$CONTRACT"
rc=$?
if [ "$rc" -eq 6 ]; then
    echo "PASS fr_w1_trusted_processes_refuse_to_start_without_the_watchdog (arm exit $rc)"
else
    echo "FAIL fr_w1_trusted_processes_refuse_to_start_without_the_watchdog (arm exit $rc)"
    fail=1
fi

echo "## the arm alone (test_arm plays the guardian)"
arb_wd_up
arb_arm_up
ls -l /dev/name/local/arbiter_arm /dev/shmem/arbiter_arm_telemetry
pidin -p arb-arm -F "%a %b %p %n"
if pidin -p arb-arm -F "%b %p" | grep -q ' 70f'; then
    echo "PASS nfr_i3_arm_runs_fifo_at_its_documented_priority (70, FIFO)"
else
    echo "FAIL nfr_i3_arm_runs_fifo_at_its_documented_priority"
    fail=1
fi
./test_arm "$WD" || fail=1
arb_down

echo "## guardian and arm end to end"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e move || fail=1
arb_down

echo "## guardian killed mid-move"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e killed "$GUARD" || fail=1
arb_down

echo "## guardian stopped mid-move"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e stopped "$GUARD" || fail=1
arb_down

echo "## arm killed: the watchdog puts the guardian in SAFE_STOP"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e armkilled "$ARM" || fail=1
arb_down

echo "## FR-W5: the watchdog killed mid-move"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e wdkilled "$WD" || fail=1
arb_down

echo "## FR-W6: the watchdog stopped mid-move, then resumed"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e wdstopped "$WD" || fail=1
arb_down

echo "## FR-W6: the watchdog stopped while its beat is forged"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e wdforged "$WD" || fail=1
arb_down

echo "## NFR-Q6: trusted processes log through slogger2"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e log "$GUARD" "$ARM" "$WD" || fail=1
arb_down

# Last, because it stops the system logger. The QEMU image starts slogger2 at
# boot; it is restarted here with its original arguments, and the suite fails if
# that does not work.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/slogger2.html
echo "## NFR-Q6: slogger2 not running: the trusted processes still start and run"
set -- $(pidin -P slogger2 arg | tail -1)
SLOG_PID=$1
shift 2 2>/dev/null                     # the pid and the program name; the rest are its arguments
SLOG_ARGS="$*"
echo "slogger2: pid $SLOG_PID, arguments '$SLOG_ARGS'"
case "$SLOG_PID" in
    ''|*[!0-9]*)
        echo "FAIL nfr_q6_trusted_processes_run_without_slogger2 (slogger2 not found, cannot restore it)"
        fail=1
        ;;
    *)
        kill "$SLOG_PID"
        for i in 1 2 3 4 5 6 7 8 9 10; do     # bound: 10 x 0.2 s
            kill -0 "$SLOG_PID" 2>/dev/null || break
            sleep 0.2
        done
        q6=0
        if kill -0 "$SLOG_PID" 2>/dev/null; then
            echo "slogger2 did not stop"
            q6=1
        fi
        ./arb-watchdog 2>wd.err &
        WD=$!
        sleep 1
        ./arb-arm "$CONTRACT" 2>arm.err &
        ARM=$!
        sleep 1
        ./arb-guardian "$CONTRACT" 2>guard.err &
        GUARD=$!
        sleep 1
        for f in wd.err arm.err guard.err; do
            cat "$f"
            grep -q "running without the system log" "$f" || { echo "$f: no 'running without the system log'"; q6=1; }
        done
        ./test_e2e move || q6=1
        arb_down
        rm -f wd.err arm.err guard.err
        if [ "$q6" -eq 0 ]; then
            echo "PASS nfr_q6_trusted_processes_run_without_slogger2"
        else
            echo "FAIL nfr_q6_trusted_processes_run_without_slogger2"
            fail=1
        fi
        # Restore the logger and prove it works: a fresh watchdog's ready line must be readable.
        # Detached from this session (stdin, stdout and stderr), or the SSH session that
        # runs the suite stays open after the suite ends.
        /proc/boot/slogger2 $SLOG_ARGS </dev/null >/dev/null 2>&1 &
        sleep 1
        ./arb-watchdog &
        WD=$!
        sleep 1
        if slog2info -b arb_watchdog | grep "arb_watchdog.$WD " | grep -q "ready as"; then
            echo "slogger2 restored: $(pidin -P slogger2 arg | tail -1)"
        else
            echo "FAIL slogger2 was not restored; reboot the target"
            fail=1
        fi
        kill "$WD"; wait "$WD" 2>/dev/null
        WD=
        ;;
esac

if [ "$fail" -eq 0 ]; then
    echo "RESULT PASS arm_suite"
else
    echo "RESULT FAIL arm_suite"
fi
exit "$fail"
