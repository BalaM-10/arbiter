#!/bin/sh
# Target side of the scripted-planner end-to-end test (host/arbiter/planner_e2e.py,
# FR-P1): start the watchdog, the arm, the guardian, the evidence logger and the
# gateway, each as its own type on the policy image; wait while the planner on the
# host plays its path through the gateway, until the host creates $DIR/done
# (bounded: 150 s); then report the guardian's status, which processes are still
# running, and the evidence log (EVLOG lines) with its anchor, and stop everything.
#   ./planner_e2e.sh <contract.bin>
cd "$(dirname "$0")" || exit 1
. ./arb_suite_lib.sh
CONTRACT="${1:?usage: planner_e2e.sh <contract.bin>}"
DIR=/tmp/arbiter-planner
for p in arb-gateway arb-logd arb-guardian arb-arm arb-watchdog; do
    slay -f "$p" >/dev/null 2>&1
done
rm -rf "$DIR"
mkdir -p "$DIR"

$ARB_ON_WD ./arb-watchdog </dev/null >"$DIR/watchdog.out" 2>&1 &
sleep 1
$ARB_ON_ARM ./arb-arm "$CONTRACT" </dev/null >"$DIR/arm.out" 2>&1 &
sleep 1
$ARB_ON_GUARD ./arb-guardian "$CONTRACT" </dev/null >"$DIR/guardian.out" 2>&1 &
sleep 1
$ARB_ON_LOGD ./arb-logd "$CONTRACT" "$DIR/evidence.log" </dev/null >"$DIR/logd.out" 2>&1 &
sleep 1
$ARB_ON_GW ./arb-gateway </dev/null >"$DIR/gateway.out" 2>&1 &
sleep 1
cat "$DIR/arm.out" "$DIR/guardian.out" "$DIR/logd.out" "$DIR/gateway.out"
echo "READY"

i=0
while [ ! -e "$DIR/done" ] && [ "$i" -lt 300 ]; do     # bound: 300 x 0.5 s
    sleep 0.5
    i=$((i + 1))
done

echo "STATUS $(./arb-ctl status | head -1)"
alive() { [ -n "$(pid_of "$1")" ] && echo 1 || echo 0; }
echo "ALIVE watchdog $(alive arb-watchdog) arm $(alive arb-arm) guardian $(alive arb-guardian) logd $(alive arb-logd) gateway $(alive arb-gateway)"
slay -f arb-gateway >/dev/null 2>&1
slay -f arb-logd >/dev/null 2>&1          # SIGTERM: the logger writes its end record
sleep 1
sed 's/^/EVLOG /' "$DIR/evidence.log"
echo "ANCHOR $(cat "$DIR/evidence.log.anchor")"
for p in arb-guardian arb-arm arb-watchdog; do
    slay -f "$p" >/dev/null 2>&1
done
sleep 1
