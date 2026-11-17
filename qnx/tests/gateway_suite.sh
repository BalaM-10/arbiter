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
    # The one address it listens on, from its ready line ("ready, listening on <addr> ...").
    GWADDR=$(sed -n 's/.*ready, listening on \([0-9.]*\) .*/\1/p' /tmp/arb-gateway.out | head -1)
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
# FR-N1: by default the gateway listens on the target's own wired interface (the
# first interface that is up and running, not loopback, with an IPv4 address),
# on that one address and never on every address (THREAT_MODEL A2).
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/i/ifconfig.html
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/n/netstat.html
c=0
ifaddr=$(ifconfig | awk '/^[a-z]/ {ok = ($0 ~ /<UP/) && ($0 ~ /RUNNING/) && ($0 !~ /LOOPBACK/)}
    ok && $1 == "inet" && !done {print $2; done = 1}')
echo "  the target's wired IPv4 address: ${ifaddr:-none}; the gateway listens on: ${GWADDR:-none}"
[ -n "$GWADDR" ] && [ "$GWADDR" = "$ifaddr" ] || c=1
socks=$(netstat -an | grep -E '[.](7700|7701) .*LISTEN')
printf '%s\n' "$socks"
if printf '%s\n' "$socks" | grep -q '[*][.]770'; then c=1; fi
[ "$(printf '%s\n' "$socks" | grep -c "^tcp4 .* $GWADDR[.]770[01] ")" -eq 2 ] || c=1
if [ "$c" -eq 0 ]; then
    echo "PASS fr_n1_gateway_listens_on_the_wired_interface_only"
else
    echo "FAIL fr_n1_gateway_listens_on_the_wired_interface_only"
    fail=1
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

./test_gateway run "${GWADDR:-0.0.0.0}" "${GW:-0}" "$WD" "$ARM" "$GUARD" || fail=1
down

echo "## FR-N3: the gateway killed mid-move (and FR-N4: commands without the logger)"
up
./test_gateway kill "${GWADDR:-0.0.0.0}" "${GW:-0}" "$WD" "$ARM" "$GUARD" || fail=1
down

echo "## FR-N1: the gateway refuses to listen on every address"
# Started in the background: a gateway that accepts the wildcard keeps running,
# and is then a failure (and stopped here), not a hang.
$ARB_ON_GW ./arb-gateway -a 0.0.0.0 </dev/null >/tmp/arb-gateway-any.out 2>&1 &
sleep 2
running=$(pid_of arb-gateway)
cat /tmp/arb-gateway-any.out
netstat -an | grep LISTEN | grep -v -E '[.](22|8000) '
if [ -z "$running" ] && grep -q "refused: the wildcard address" /tmp/arb-gateway-any.out; then
    echo "PASS fr_n1_gateway_refuses_the_wildcard_address"
else
    echo "FAIL fr_n1_gateway_refuses_the_wildcard_address (still running: ${running:-no})"
    fail=1
    slay -f arb-gateway >/dev/null 2>&1
fi

if [ "$fail" -eq 0 ]; then
    echo "RESULT PASS gateway_suite"
else
    echo "RESULT FAIL gateway_suite"
fi
exit "$fail"
