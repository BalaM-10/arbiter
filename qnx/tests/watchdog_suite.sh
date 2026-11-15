#!/bin/sh
# On-target watchdog tests: each test runs against a freshly started arb-watchdog.
# Exit status 0 only if every case passed.
cd "$(dirname "$0")" || exit 1
fail=0

echo "## watchdog priority"
./arb-watchdog 0 0 &     # test_watchdog plays both roles as root
WD=$!
sleep 1
ls -l /dev/name/local/arbiter_watchdog /dev/shmem/arbiter_watchdog_status
pidin -p arb-watchdog -F "%a %b %p %n"
if pidin -p arb-watchdog -F "%b %p" | grep -q ' 66f'; then
    echo "PASS fr_w1_watchdog_runs_above_the_guardian_and_below_the_arm (66, FIFO)"
else
    echo "FAIL fr_w1_watchdog_runs_above_the_guardian_and_below_the_arm"
    fail=1
fi
kill "$WD"; wait "$WD" 2>/dev/null; sleep 1

for t in $(./test_watchdog list); do
    echo "## $t"
    ./arb-watchdog 0 0 &     # test_watchdog plays both roles as root
    WD=$!
    sleep 1
    ./test_watchdog "$t" || fail=1
    kill "$WD"; wait "$WD" 2>/dev/null
    sleep 1
done

if [ "$fail" -eq 0 ]; then
    echo "RESULT PASS watchdog_suite"
else
    echo "RESULT FAIL watchdog_suite"
fi
exit "$fail"
