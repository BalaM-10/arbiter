#!/bin/sh
# On-target arm controller tests (run from the deploy directory by scripts/run-tests.sh):
# the arm alone with test_arm playing the guardian, then the real guardian and the
# real arm end to end. Exit status 0 only if every case passed.
cd "$(dirname "$0")" || exit 1
. ./arb_suite_lib.sh
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

arb_arm_up() {      # [guardian-uid]: only the arm-alone tests, where test_arm plays the guardian as root
    ./arb-arm "$CONTRACT" ${1:+"$1"} &
    ARM=$!
    sleep 1
}

OPERATOR_UID=800                # FR-G5: the operator's user ID (test_e2e.c OPERATOR_UID)

arb_guard_up() {
    ./arb-guardian "$CONTRACT" "$OPERATOR_UID" &
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
# Whatever happens, leave no Arbiter process behind: one would keep the SSH
# session (and run-tests.sh) open.
trap 'arb_down; slay -f arb-guardian arb-arm arb-watchdog >/dev/null 2>&1' EXIT

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

echo "## the arm alone (test_arm plays the guardian, as root)"
arb_wd_up
arb_arm_up 0
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

# NFR-I5, after the client traffic above. Each trusted process: its own user and
# group (qnx/common/arb_ids.h), still FIFO at its arb_prio.h priority, and its own
# startup check that what it dropped is refused (arb_privdrop.c), read from slog2.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/p/pidin.html
echo "## NFR-I5: own users, priorities kept, dropped abilities refused"
for n in arb-watchdog arb-arm arb-guardian; do pidin -p "$n" -F "%a %U %V %b %p %n"; done
i5p=0
i5d=0
i5_check arb-watchdog arb_watchdog "$WD" 812 66 "$I5_WD_LINE"
i5_check arb-arm arb_arm "$ARM" 811 70 "$I5_ARM_LINE"
i5_check arb-guardian arb_guardian "$GUARD" 810 64 "$I5_GUARD_LINE"
if [ "$i5p" -eq 0 ]; then
    echo "PASS nfr_i5_trusted_processes_keep_their_priority_after_dropping_privileges"
else
    echo "FAIL nfr_i5_trusted_processes_keep_their_priority_after_dropping_privileges"
    fail=1
fi
if [ "$i5d" -eq 0 ]; then
    echo "PASS nfr_i5_dropped_abilities_are_denied"
else
    echo "FAIL nfr_i5_dropped_abilities_are_denied"
    fail=1
fi
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

# Midpoint review M2: beats and trips count only from the watchdog's own
# connection. test_wd_scoid plays the watchdog itself and starts the real arm
# and guardian, so no arb_wd_up here.
echo "## FR-W6, FR-W2: beats and trips only from the watchdog's connection (stand-in watchdog)"
./test_wd_scoid "$CONTRACT" || fail=1
slay -f arb-guardian arb-arm >/dev/null 2>&1
sleep 1

echo "## FR-W4, FR-G5: e-stop from any client, reset only from the operator, not without a watchdog"
arb_wd_up
arb_arm_up
arb_guard_up
./test_e2e operator "$WD" || fail=1
# FR-G5 with FR-W6: during the repeated resets, the arm and the guardian each
# recognise beats the watchdog delivered under their previous registration (still
# queued when they registered again) as stale, not forged, and log it.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/slog2info.html
c=0
for spec in "arb_arm $ARM" "arb_guardian $GUARD"; do
    set -- $spec
    line=$(slog2info -b "$1" | grep "$1.$2 " | grep "of the previous registration" | tail -1)
    if [ -n "$line" ]; then echo "  ok   $1: $line"; else echo "  FAIL $1: no stale-event line in slog2"; c=1; fi
done
if [ "$c" -eq 0 ]; then
    echo "PASS fr_g5_stale_watchdog_events_after_a_reset_are_recognised"
else
    echo "FAIL fr_g5_stale_watchdog_events_after_a_reset_are_recognised"
    fail=1
fi
arb_down

# The operator console. uid 800 cannot enter root's home, where the binaries are,
# so the operator's copy runs from /tmp.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/o/on.html
echo "## FR-W4, FR-G5: the operator console arb-ctl"
arb_wd_up
arb_arm_up
arb_guard_up
CTL=/tmp/arb-ctl.$$
cp ./arb-ctl "$CTL" && chmod 755 "$CTL"
c=0
./arb-ctl estop || c=1
./arb-ctl status | grep -q "^state SAFE_STOP cause estop" || c=1
if ./arb-ctl reset; then c=1; fi                          # root is not the operator
on -u "$OPERATOR_UID" "$CTL" reset || c=1
./arb-ctl status | grep -q "^state HOLD cause none estops 1 resets 1 resets_refused 1 " || c=1
rm -f "$CTL"
if [ "$c" -eq 0 ]; then
    echo "PASS fr_w4_operator_console_estop_and_reset"
else
    echo "FAIL fr_w4_operator_console_estop_and_reset"
    fail=1
fi
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
