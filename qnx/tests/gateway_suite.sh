#!/bin/sh
# On-target gateway tests (run from the deploy directory by scripts/run-tests.sh):
# the watchdog, the arm, the guardian and arb-gateway, with test_gateway playing
# the planner and the observers over TCP (docs/PROTOCOL.md). On the policy image
# every process starts as its own type (arb_suite_lib.sh).
# Exit status 0 only if every case passed.
cd "$(dirname "$0")" || exit 1
. ./arb_suite_lib.sh
CONTRACT=contract-corpus/contract.bin
fail=0

EV=/tmp/arbiter-gateway-test

up() {             # up [logd]: with "logd", also the evidence logger, which relays telemetry (D1)
    $ARB_ON_WD ./arb-watchdog </dev/null >/dev/null 2>&1 &
    sleep 1
    $ARB_ON_ARM ./arb-arm "$CONTRACT" </dev/null >/dev/null 2>&1 &
    sleep 1
    $ARB_ON_GUARD ./arb-guardian "$CONTRACT" </dev/null >/dev/null 2>&1 &
    sleep 1
    if [ "${1:-}" = logd ]; then
        rm -rf "$EV"
        mkdir -p "$EV"
        $ARB_ON_LOGD ./arb-logd "$CONTRACT" "$EV/evidence.log" </dev/null >"$EV/logd.out" 2>&1 &
        sleep 1
    fi
    $ARB_ON_GW ./arb-gateway </dev/null >/tmp/arb-gateway.out 2>&1 &
    sleep 1
    WD=$(pid_of arb-watchdog)
    ARM=$(pid_of arb-arm)
    GUARD=$(pid_of arb-guardian)
    GW=$(pid_of arb-gateway)
}

down() {
    for p in arb-gateway arb-logd arb-guardian arb-arm arb-watchdog; do
        slay -f "$p" >/dev/null 2>&1
    done
    sleep 1
}

echo "## FR-N1, FR-N2, FR-N4, FR-G10, FR-W4, NFR-I1: the gateway, from a planner and observers, with the logger"
down
up logd
echo "pids: watchdog $WD arm $ARM guardian $GUARD logger $(pid_of arb-logd) gateway ${GW:-none}"
cat /tmp/arb-gateway.out
ls -l /dev/shmem/arbiter_arm_telemetry /dev/shmem/arbiter_decisions
if [ -z "$GW" ]; then
    echo "FAIL arb-gateway is not running"
    fail=1
fi

# NFR-I5 for the gateway: its own user and group, FIFO at priority 10 (below every
# trusted process), its drop line in slog2, and no new socket after the drop.
i5p=0
i5d=0
if [ -n "$GW" ]; then
    i5_check arb-gateway arb_gateway "$GW" 814 10 "$I5_GW_LINE"
    if slog2info -b arb_gateway | grep "arb_gateway.$GW " | grep -qF "$I5_GW_SOCKET_LINE"; then
        echo "  ok   arb-gateway: $I5_GW_SOCKET_LINE"
    else
        echo "  FAIL arb-gateway: no slog2 line '$I5_GW_SOCKET_LINE'"
        i5d=1
    fi
else
    i5p=1
fi
if [ "$i5p" -eq 0 ] && [ "$i5d" -eq 0 ]; then
    echo "PASS nfr_i5_gateway_runs_as_its_own_user_with_no_network_ability"
else
    echo "FAIL nfr_i5_gateway_runs_as_its_own_user_with_no_network_ability"
    fail=1
fi

# FR-S1 on the policy image: the gateway runs as its own type.
if [ "$ARB_POLICY_IMAGE" -eq 1 ]; then
    if [ -n "$GW" ] && pidin -f a_n | awk -v p="$GW" '$1 == p && $2 == "arb_gateway_t" {f = 1} END {exit !f}'; then
        echo "PASS fr_s1_the_gateway_runs_as_its_own_type (pid $GW, arb_gateway_t)"
    else
        echo "FAIL fr_s1_the_gateway_runs_as_its_own_type"
        fail=1
    fi
fi

./test_gateway run "${GW:-0}" "$WD" "$ARM" "$GUARD" || fail=1
down

echo "## FR-N3: the gateway killed mid-move (and FR-N4: commands without the logger)"
up
./test_gateway kill "${GW:-0}" "$WD" "$ARM" "$GUARD" || fail=1
down

if [ "$fail" -eq 0 ]; then
    echo "RESULT PASS gateway_suite"
else
    echo "RESULT FAIL gateway_suite"
fi
exit "$fail"
