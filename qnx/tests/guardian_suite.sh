#!/bin/sh
# On-target guardian tests (run from the deploy directory by scripts/run-tests.sh).
# Starts the stub arm and arb-guardian in each mode and runs test_guardian.
# Exit status 0 only if every case passed.
cd "$(dirname "$0")" || exit 1
CONTRACT=contract-corpus/contract.bin
fail=0
STUB=
GUARD=

arb_start() {    # arb_start <stub mode>   (watchdog, stand-in arm, guardian)
    ./arb-watchdog &
    WD=$!
    sleep 1
    ./arb_arm_stub "$1" &
    STUB=$!
    sleep 1
    ./arb-guardian "$CONTRACT" &
    GUARD=$!
    sleep 1
}

arb_stop() {
    kill "$GUARD" "$STUB" "$WD" 2>/dev/null
    wait "$GUARD" 2>/dev/null
    wait "$STUB" 2>/dev/null
    wait "$WD" 2>/dev/null
    sleep 1
}

arb_refuses() {  # arb_refuses <table> : FR-C4, the guardian must exit 2 and never attach /dev/arbiter
    ./arb-guardian "$1"
    rc=$?
    if [ "$rc" -eq 2 ] && [ ! -e /dev/arbiter ]; then
        echo "PASS fr_c4_guardian_refuses_corrupted_contract $1 (exit $rc, no /dev/arbiter)"
    else
        echo "FAIL fr_c4_guardian_refuses_corrupted_contract $1 (exit $rc)"
        fail=1
    fi
}

echo "## FR-C4: the guardian refuses to start on a bad contract table"
arb_refuses contract-corpus/crc.bin
arb_refuses contract-corpus/truncated.bin
arb_refuses contract-corpus/version.bin
arb_refuses contract-corpus/no-such-file.bin

echo "## well-behaved arm"
arb_start normal
ls -l /dev/arbiter /dev/name/local/arbiter_arm
if [ -e /dev/arbiter ] && ls -l /dev/arbiter | grep -q '^n'; then
    echo "PASS nfr_q3_guardian_is_a_resource_manager_in_the_path_namespace"
else
    echo "FAIL nfr_q3_guardian_is_a_resource_manager_in_the_path_namespace"
    fail=1
fi
pidin -p arb-guardian -F "%a %b %p %n"
if pidin -p arb-guardian -F "%b %p" | grep -q ' 64f'; then
    echo "PASS nfr_i3_guardian_runs_fifo_at_its_documented_priority (64, FIFO)"
else
    echo "FAIL nfr_i3_guardian_runs_fifo_at_its_documented_priority"
    fail=1
fi
./test_guardian normal || fail=1
arb_stop

echo "## arm receives setpoints and never replies (REPLY-blocked guardian)"
arb_start hang
./test_guardian hang || fail=1
arb_stop

echo "## arm process stopped (SEND-blocked guardian)"
arb_start normal
kill -STOP "$STUB"
./test_guardian stopped || fail=1
kill -CONT "$STUB"
arb_stop

echo "## arm rejects the start position"
arb_start stale
./test_guardian stale || fail=1
arb_stop

if [ "$fail" -eq 0 ]; then
    echo "RESULT PASS guardian_suite"
else
    echo "RESULT FAIL guardian_suite"
fi
exit "$fail"
