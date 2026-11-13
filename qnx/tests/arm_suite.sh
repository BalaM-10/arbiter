#!/bin/sh
# On-target arm controller tests (run from the deploy directory by scripts/run-tests.sh):
# the arm alone with test_arm playing the guardian, then the real guardian and the
# real arm end to end. Exit status 0 only if every case passed.
cd "$(dirname "$0")" || exit 1
CONTRACT=contract-corpus/contract.bin
fail=0
ARM=
GUARD=

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
    kill "$GUARD" "$ARM" 2>/dev/null
    kill -CONT "$GUARD" 2>/dev/null
    wait "$GUARD" 2>/dev/null
    wait "$ARM" 2>/dev/null
    GUARD=
    ARM=
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

echo "## the arm alone (test_arm plays the guardian)"
arb_arm_up
ls -l /dev/name/local/arbiter_arm /dev/shmem/arbiter_arm_telemetry
pidin -p arb-arm -F "%a %b %p %n"
if pidin -p arb-arm -F "%b %p" | grep -q ' 70f'; then
    echo "PASS nfr_i3_arm_runs_fifo_at_its_documented_priority (70, FIFO)"
else
    echo "FAIL nfr_i3_arm_runs_fifo_at_its_documented_priority"
    fail=1
fi
./test_arm || fail=1
arb_down

echo "## guardian and arm end to end"
arb_arm_up
arb_guard_up
./test_e2e move || fail=1
arb_down

echo "## guardian killed mid-move"
arb_arm_up
arb_guard_up
./test_e2e killed "$GUARD" || fail=1
arb_down

echo "## guardian stopped mid-move"
arb_arm_up
arb_guard_up
./test_e2e stopped "$GUARD" || fail=1
arb_down

if [ "$fail" -eq 0 ]; then
    echo "RESULT PASS arm_suite"
else
    echo "RESULT FAIL arm_suite"
fi
exit "$fail"
