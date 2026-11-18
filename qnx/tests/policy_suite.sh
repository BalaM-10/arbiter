#!/bin/sh
# Security-policy checks, on the policy image only (run-tests.sh qemu_secpol).
# The policy is qnx/image/policy/arbiter.secpol, compiled into the boot image and
# pushed at boot (scripts/build-image.sh qemu_secpol). This suite reads the loaded
# policy back with secpol, starts every Arbiter process as its own type, and runs
# the end-to-end cases that use only the guardian's public path. The cases that
# play the guardian or the watchdog themselves (test_arm, test_watchdog,
# guardian_suite, test_e2e wdforged and log) run on the policy-off image: here the
# root test shell may not connect to the arm or the watchdog (owner decision
# 2026-11-15). F11 with the policy on: f11_suite.sh on.
# Exit status 0 only if every case passed.
cd "$(dirname "$0")" || exit 1
. ./arb_suite_lib.sh
CONTRACT=contract-corpus/contract.bin
OPERATOR_UID=800
fail=0
WD=
ARM=
GUARD=

if [ "$ARB_POLICY_IMAGE" -ne 1 ]; then
    echo "FAIL policy_suite: not a security-policy image (no /proc/boot/secpolpush)"
    exit 1
fi

up() {
    $ARB_ON_WD ./arb-watchdog &
    WD=$!
    sleep 1
    $ARB_ON_ARM ./arb-arm "$CONTRACT" &
    ARM=$!
    sleep 1
    $ARB_ON_GUARD ./arb-guardian "$CONTRACT" "$OPERATOR_UID" &
    GUARD=$!
    sleep 1
    arb_start_check policy_suite
}
stop_pid() {       # SIGCONT and SIGTERM, then wait for it to go (bounded: 20 x 0.1 s)
    [ -n "$1" ] || return 0
    kill -CONT "$1" 2>/dev/null
    kill "$1" 2>/dev/null
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -KILL "$1" 2>/dev/null
}
down() {
    stop_pid "$GUARD"; stop_pid "$ARM"; stop_pid "$WD"
    GUARD=; ARM=; WD=
    sleep 1
}
trap 'down; slay -f arb-guardian arb-arm arb-watchdog >/dev/null 2>&1' EXIT
result() {         # result <name> <0 ok | other fail>
    if [ "$2" -eq 0 ]; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi
}

# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/secpol.html
echo "## FR-S5: the loaded policy, as the inspection tool reports it"
c=0
secpol -v || c=1
for t in arb_arm_t arb_watchdog_t arb_guardian_t arb_gateway_t; do
    echo "\$ secpol -f connect -t $t"
    secpol -f connect -t "$t" || c=1
done
echo "\$ secpol -f all   (path attach rules)"
secpol -f all | sed -n '/^SECPOL_BLOB_PMA_RULES/,/^SECPOL_BLOB_[A-Z_]*$/p' | grep -v '^SECPOL_BLOB_[A-Z_]*$' || c=1
result fr_s5_policy_inspection_output_is_in_the_run_log "$c"

echo "## FR-S4: the policy was pushed at boot and cannot be changed"
c=0
secpol -f all | grep -q "^policy_id:[0-9a-f]*  immutable$" || { echo "  FAIL no immutable policy loaded"; c=1; }
# Pushed before anything else started: every process the boot started has a type
# (the kernel, pid 1, is the only one without).
untyped=$(pidin -f a_n | awk 'NR > 1 && $1 != 1 && $2 == "-"')
if [ -n "$untyped" ]; then echo "  FAIL processes without a type:"; echo "$untyped"; c=1; else echo "  ok   every process but the kernel has a type"; fi
result fr_s4_policy_pushed_at_boot_and_immutable "$c"

echo "## FR-S2: only the guardian's type may connect to the arm"
who=$(secpol -f connect -t arb_arm_t | awk '/^[a-z_]+  crc:/ {cur = $1} /\(arb_arm_t\)/ {print cur}' | sort -u | tr '\n' ' ')
echo "  types that may connect to arb_arm_t: $who"
result fr_s2_only_the_guardians_type_may_connect_to_the_arm "$([ "$who" = "arb_guardian_t " ]; echo $?)"

echo "## FR-S2: among Arbiter's processes, the gateway's type may connect only to the guardian (and the logger, for telemetry: D1)"
# The rules listed under arb_gateway_t's own entry name the types it may connect to.
gw=$(secpol -f connect -t arb_gateway_t | awk '/^[a-z_]+  crc:/ {cur = $1}
    cur == "arb_gateway_t" && match($0, /\(arb_[a-z_]+\)/) {print substr($0, RSTART + 1, RLENGTH - 2)}' |
    sort -u | tr '\n' ' ')
echo "  Arbiter types arb_gateway_t may connect to: $gw"
result fr_s2_the_gateways_type_may_connect_only_to_the_guardian_and_the_logger "$([ "$gw" = "arb_guardian_t arb_logd_t " ]; echo $?)"

echo "## FR-S3: only the guardian's type may attach /dev/arbiter"
rules=$(secpol -f all | grep '^allow_attach .* /dev/arbiter;')
echo "  $rules"
result fr_s3_only_the_guardians_type_may_attach_dev_arbiter "$([ "$rules" = "allow_attach arb_guardian_t /dev/arbiter;" ]; echo $?)"

echo "## FR-S1: each trusted process runs as its own type"
up
c=0
for spec in "$WD arb_watchdog_t" "$ARM arb_arm_t" "$GUARD arb_guardian_t"; do
    set -- $spec
    if pidin -f a_n | awk -v p="$1" -v t="$2" '$1 == p && $2 == t {found = 1} END {exit !found}'; then
        echo "  ok   pid $1 runs as $2"
    else
        echo "  FAIL pid $1 does not run as $2"; c=1
    fi
done
result fr_s1_each_trusted_process_runs_as_its_own_type "$c"

echo "## end to end with the policy loaded, then NFR-I5 after the traffic"
./test_e2e move || fail=1
i5p=0
i5d=0
i5_check arb-watchdog arb_watchdog "$WD" 812 66 "$I5_WD_LINE"
i5_check arb-arm arb_arm "$ARM" 811 70 "$I5_ARM_LINE"
i5_check arb-guardian arb_guardian "$GUARD" 810 64 "$I5_GUARD_LINE"
result nfr_i5_trusted_processes_keep_their_priority_after_dropping_privileges "$i5p"
result nfr_i5_dropped_abilities_are_denied "$i5d"
down

for mode in killed stopped armkilled wdkilled wdstopped operator; do
    echo "## test_e2e $mode, with the policy loaded"
    up
    case "$mode" in
        killed|stopped) arg=$GUARD ;;
        armkilled) arg=$ARM ;;
        *) arg=$WD ;;
    esac
    ./test_e2e "$mode" "$arg" || fail=1
    down
done

echo "## the operator console arb-ctl, with the policy loaded"
up
CTL=/tmp/arb-ctl.$$
cp ./arb-ctl "$CTL" && chmod 755 "$CTL"
c=0
./arb-ctl estop || c=1
./arb-ctl status | grep -q "^state SAFE_STOP cause estop" || c=1
if ./arb-ctl reset; then c=1; fi                          # root is not the operator
on -u "$OPERATOR_UID" "$CTL" reset || c=1
./arb-ctl status | grep -q "^state HOLD cause none estops 1 resets 1 resets_refused 1 " || c=1
rm -f "$CTL"
result fr_w4_operator_console_estop_and_reset "$c"
down

if [ "$fail" -eq 0 ]; then
    echo "RESULT PASS policy_suite"
else
    echo "RESULT FAIL policy_suite"
fi
exit "$fail"
