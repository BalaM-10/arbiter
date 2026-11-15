#!/bin/sh
# F11 bypass attempt, on target (run from the deploy directory by scripts/run-tests.sh).
#   f11_suite.sh [off|on]     the security-policy mode of the image (default off)
#
# The attacker is arb-untrusted, run as user 820 (ARB_UID_UNTRUSTED), never root;
# with the policy on, also as type arb_untrusted_t. Three cases, each judged here
# from arb-untrusted's step results and the arm's own counters:
#   1. it connects to the arm and sends a setpoint while the guardian is up;
#   2. it starts before the guardian, connects and tries to bind the arm first;
#   3. it tries to attach /dev/arbiter before the guardian starts;
#   4. it registers the arm's (4a) or the watchdog's (4b) name before the real
#      one, to be the server that the trusted clients connect to.
# Policy off: the connects succeed and the arm refuses (not the guardian's user
# ID): rejected moves, the refusal is in slog2, and the real guardian still binds.
# The attach is refused by the kernel: the attacker lacks PATHSPACE (not root).
# Policy on: the connect or attach fails in the kernel, the arm's rejected counter
# does not move (no Arbiter code ran), and the guardian then binds normally.
# Exit status 0 only if every case passed.
cd "$(dirname "$0")" || exit 1
MODE=${1:-off}
case "$MODE" in off|on) ;; *) echo "usage: $0 [off|on]"; exit 2 ;; esac
CONTRACT=contract-corpus/contract.bin
UNTRUSTED_UID=820                      # ARB_UID_UNTRUSTED (qnx/common/arb_ids.h)
fail=0
WD=
ARM=
GUARD=

# With the policy on, each process gets its type; the trusted ones start as root
# and drop to their own user themselves (NFR-I5).
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/o/on.html
typed() {          # typed <type> <command...>
    t=$1
    shift
    if [ "$MODE" = on ]; then on -T "$t" "$@"; else "$@"; fi
}

# The user 820 cannot enter root's home, where the binaries are: its copy runs from /tmp.
ATK=/tmp/arb-untrusted.$$
cp ./arb-untrusted "$ATK" && chmod 755 "$ATK" || { echo "FAIL cannot copy arb-untrusted to /tmp"; exit 1; }
attacker() {
    if [ "$MODE" = on ]; then
        on -T arb_untrusted_t -u "$UNTRUSTED_UID:$UNTRUSTED_UID" "$ATK" "$@"
    else
        on -u "$UNTRUSTED_UID:$UNTRUSTED_UID" "$ATK" "$@"
    fi
}

# A process started through a shell function (or through on) is not the shell's
# direct child, so $! is not its pid: look the pid up by name instead.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/p/pidin.html
pid_of() { pidin -p "$1" -F "%a" 2>/dev/null | tail -n +2 | tail -1 | tr -d ' '; }
wd_up()    { typed arb_watchdog_t ./arb-watchdog & sleep 1; WD=$(pid_of arb-watchdog); }
arm_up()   { typed arb_arm_t ./arb-arm "$CONTRACT" & sleep 1; ARM=$(pid_of arb-arm); }
guard_up() { typed arb_guardian_t ./arb-guardian "$CONTRACT" & sleep 1; GUARD=$(pid_of arb-guardian); }
stop_pid() {       # stop_pid <pid>: SIGTERM, then wait for it to go (bounded: 20 x 0.1 s)
    [ -n "$1" ] || return 0
    kill "$1" 2>/dev/null
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -KILL "$1" 2>/dev/null
}
all_down() {
    stop_pid "$GUARD"
    stop_pid "$ARM"
    stop_pid "$WD"
    GUARD=
    ARM=
    WD=
    sleep 1
}
# Whatever happens, leave no Arbiter process behind: one would keep the SSH
# session (and run-tests.sh) open.
cleanup() {
    all_down
    for n in arb-guardian arb-arm arb-watchdog; do stop_pid "$(pid_of "$n")"; done
    [ -n "$SQPID" ] && kill "$SQPID" 2>/dev/null
    rm -f "$ATK" /tmp/f11-bind.$$ /tmp/f11-squat.$$ /tmp/f11-arm.$$ /tmp/f11-guard.$$
}
SQPID=
trap cleanup EXIT

# field <output> <line prefix> <word after which the value follows>
field() {
    printf '%s\n' "$1" | grep "^$2" | head -1 | awk -v k="$3" '{for (i = 1; i < NF; i++) if ($i == k) { print $(i + 1); exit }}'
}
check() {          # check <case> <description> <condition-exit-status>
    if [ "$3" -eq 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; c=1; fi
}
refused_in_slog() {   # the arm logged a refused sender
    slog2info -b arb_arm | grep "arb_arm.$ARM " | grep -q "refused a sender"
}

echo "## F11 case 1: an untrusted process sends a setpoint to the arm while the guardian is up (policy $MODE)"
c=0
wd_up; arm_up; guard_up
out=$(attacker setpoint)
printf '%s\n' "$out"
r0=$(field "$out" "COUNTERS before" rejected)
r1=$(field "$out" "COUNTERS after" rejected)
b1=$(field "$out" "COUNTERS after" bound_pid)
check 1 "the attacker ran as user $UNTRUSTED_UID" "$(printf '%s\n' "$out" | grep -q "^ARB-UNTRUSTED setpoint pid [0-9]* uid $UNTRUSTED_UID euid $UNTRUSTED_UID$"; echo $?)"
if [ "$MODE" = off ]; then
    check 1 "connect succeeds (no policy)" "$(printf '%s\n' "$out" | grep -q '^STEP name_open rc [0-9]'; echo $?)"
    check 1 "the arm refuses the setpoint with EPERM" "$(printf '%s\n' "$out" | grep -q '^STEP setpoint rc -1 errno 1 '; echo $?)"
    check 1 "rejected +1 ($r0 -> $r1)" "$([ -n "$r0" ] && [ "$r1" -eq $((r0 + 1)) ]; echo $?)"
    check 1 "the refusal is in slog2" "$(refused_in_slog; echo $?)"
else
    check 1 "connect fails in the kernel" "$(printf '%s\n' "$out" | grep -q '^STEP name_open rc -1 '; echo $?)"
    check 1 "rejected unchanged ($r0 -> $r1): no Arbiter code ran" "$([ -n "$r0" ] && [ "$r1" -eq "$r0" ]; echo $?)"
fi
check 1 "the arm stays bound to the guardian (pid $GUARD)" "$([ "$b1" = "$GUARD" ]; echo $?)"
all_down
if [ "$c" -eq 0 ]; then echo "PASS f11_untrusted_setpoint_to_the_arm_is_refused ($MODE)"; else echo "FAIL f11_untrusted_setpoint_to_the_arm_is_refused ($MODE)"; fail=1; fi

echo "## F11 case 2: the untrusted process starts before the guardian and tries to bind the arm first (policy $MODE)"
c=0
wd_up; arm_up
attacker bind 3000 >/tmp/f11-bind.$$ 2>&1 &
ATKPID=$!
sleep 1                                 # the attacker has connected and tried to bind
guard_up
wait "$ATKPID"
out=$(cat /tmp/f11-bind.$$)
rm -f /tmp/f11-bind.$$
printf '%s\n' "$out"
r0=$(field "$out" "COUNTERS before" rejected)
r1=$(field "$out" "COUNTERS after" rejected)
b0=$(field "$out" "COUNTERS before" bound_pid)
b1=$(field "$out" "COUNTERS after" bound_pid)
bh=$(field "$out" "COUNTERS held" bound_pid)
check 2 "the arm was unbound when the attacker came ($b0)" "$([ "$b0" = "-1" ]; echo $?)"
if [ "$MODE" = off ]; then
    check 2 "connect succeeds (no policy)" "$(printf '%s\n' "$out" | grep -q '^STEP name_open rc [0-9]'; echo $?)"
    check 2 "the binding message is refused with EPERM" "$(printf '%s\n' "$out" | grep -q '^STEP get_state rc -1 errno 1 '; echo $?)"
    check 2 "the setpoint is refused with EPERM" "$(printf '%s\n' "$out" | grep -q '^STEP setpoint rc -1 errno 1 '; echo $?)"
    check 2 "rejected +2 ($r0 -> $r1)" "$([ -n "$r0" ] && [ "$r1" -eq $((r0 + 2)) ]; echo $?)"
    check 2 "the refusal is in slog2" "$(refused_in_slog; echo $?)"
else
    check 2 "connect fails in the kernel" "$(printf '%s\n' "$out" | grep -q '^STEP name_open rc -1 '; echo $?)"
    check 2 "rejected unchanged ($r0 -> $r1): no Arbiter code ran" "$([ -n "$r0" ] && [ "$r1" -eq "$r0" ]; echo $?)"
fi
check 2 "the attacker bound nothing ($b1)" "$([ "$b1" = "-1" ]; echo $?)"
check 2 "the guardian started later and bound the arm (pid $GUARD, bound $bh)" "$([ "$bh" = "$GUARD" ]; echo $?)"
all_down
if [ "$c" -eq 0 ]; then echo "PASS f11_untrusted_started_first_cannot_bind_the_arm ($MODE)"; else echo "FAIL f11_untrusted_started_first_cannot_bind_the_arm ($MODE)"; fail=1; fi

echo "## F11 case 3: the untrusted process tries to attach /dev/arbiter before the guardian starts (policy $MODE)"
c=0
wd_up; arm_up
out=$(attacker attach)
printf '%s\n' "$out"
check 3 "resmgr_attach is refused by the kernel" "$(printf '%s\n' "$out" | grep -q '^STEP resmgr_attach rc -1 '; echo $?)"
if [ "$MODE" = off ]; then
    check 3 "with EPERM (no PATHSPACE ability: not root)" "$(printf '%s\n' "$out" | grep -q '^STEP resmgr_attach rc -1 errno 1 '; echo $?)"
fi
check 3 "no /dev/arbiter before the guardian" "$([ ! -e /dev/arbiter ]; echo $?)"
guard_up
ls -l /dev/arbiter
check 3 "the guardian then attaches /dev/arbiter and runs" "$([ -e /dev/arbiter ] && kill -0 "$GUARD" 2>/dev/null; echo $?)"
all_down
if [ "$c" -eq 0 ]; then echo "PASS f11_untrusted_cannot_attach_dev_arbiter_first ($MODE)"; else echo "FAIL f11_untrusted_cannot_attach_dev_arbiter_first ($MODE)"; fail=1; fi

# Case 4: name squatting. The attacker registers a trusted process's name first.
# Policy off: the real process cannot register the taken name and exits; every
# trusted client checks who serves a name before sending it anything
# (arb_server_check.c), sees a user it may not even query, and refuses to start;
# the attacker receives no message. Policy on: the registration itself is refused.
squat_bg() {       # squat_bg <arm|watchdog> <hold_ms> <outfile>: start the attacker, give it time to register
    attacker squat "$1" "$2" >"$3" 2>&1 &
    SQPID=$!
    sleep 1
}
squat_end() {      # squat_end <outfile>: wait for the attacker's hold to end; print its output
    wait "$SQPID"
    out=$(cat "$1")
    rm -f "$1"
    printf '%s\n' "$out"
}
received_nothing() {   # the attacker's channel saw no message (connect messages and pulses aside)
    printf '%s\n' "$out" | grep -q '^RECEIVED connects [0-9]* messages 0 pulses [0-9]*$'
}

echo "## F11 case 4a: the untrusted process registers the arm's name before the arm starts (policy $MODE)"
c=0
SQ=/tmp/f11-squat.$$
wd_up
if [ "$MODE" = off ]; then
    squat_bg arm 4000 "$SQ"
    ./arb-arm "$CONTRACT" 2>/tmp/f11-arm.$$; rc=$?
    cat /tmp/f11-arm.$$
    check 4a "the real arm cannot register its taken name and exits (5): fail-safe, nothing moves" "$([ "$rc" -eq 5 ]; echo $?)"
    ./arb-guardian "$CONTRACT" 2>/tmp/f11-guard.$$; rc=$?
    cat /tmp/f11-guard.$$
    check 4a "the guardian refuses to start (4)" "$([ "$rc" -eq 4 ]; echo $?)"
    check 4a "because the server of arbiter_arm is not the arm's user" "$(grep -q 'the server of arbiter_arm is pid [0-9]*, user -1, not user 811: refused' /tmp/f11-guard.$$; echo $?)"
    rm -f /tmp/f11-arm.$$ /tmp/f11-guard.$$
    squat_end "$SQ"
    check 4a "the attacker registered the name (no policy)" "$(printf '%s\n' "$out" | grep -q '^STEP name_attach rc 0 '; echo $?)"
    check 4a "the attacker received no message from the guardian" "$(received_nothing; echo $?)"
else
    out=$(attacker squat arm 100)
    printf '%s\n' "$out"
    check 4a "the attacker cannot register the arm's name: refused by the policy" "$(printf '%s\n' "$out" | grep -q '^STEP name_attach rc -1 '; echo $?)"
    arm_up; guard_up
    check 4a "the real arm and guardian then start and run" "$(kill -0 "$ARM" 2>/dev/null && kill -0 "$GUARD" 2>/dev/null; echo $?)"
fi
all_down
if [ "$c" -eq 0 ]; then echo "PASS f11_untrusted_cannot_impersonate_the_arm_by_its_name ($MODE)"; else echo "FAIL f11_untrusted_cannot_impersonate_the_arm_by_its_name ($MODE)"; fail=1; fi

echo "## F11 case 4b: the untrusted process registers the watchdog's name (policy $MODE)"
c=0
if [ "$MODE" = off ]; then
    # Before any watchdog: the arm checks who serves the name, and refuses to start.
    squat_bg watchdog 3000 "$SQ"
    ./arb-arm "$CONTRACT" 2>/tmp/f11-arm.$$; rc=$?
    cat /tmp/f11-arm.$$
    check 4b "the arm refuses to start (6)" "$([ "$rc" -eq 6 ]; echo $?)"
    check 4b "because the server of arbiter_watchdog is not the watchdog's user" "$(grep -q 'the server of arbiter_watchdog is pid [0-9]*, user -1, not user 812: refused' /tmp/f11-arm.$$; echo $?)"
    squat_end "$SQ"
    check 4b "the attacker received no message from the arm" "$(received_nothing; echo $?)"
    # The real watchdog dies and the attacker takes its name: a guardian started now refuses too.
    wd_up; arm_up
    stop_pid "$WD"; WD=
    squat_bg watchdog 3000 "$SQ"
    ./arb-guardian "$CONTRACT" 2>/tmp/f11-guard.$$; rc=$?
    cat /tmp/f11-guard.$$
    check 4b "a guardian started after the watchdog died refuses to start (6)" "$([ "$rc" -eq 6 ]; echo $?)"
    check 4b "because the server of arbiter_watchdog is not the watchdog's user" "$(grep -q 'the server of arbiter_watchdog is pid [0-9]*, user -1, not user 812: refused' /tmp/f11-guard.$$; echo $?)"
    squat_end "$SQ"
    check 4b "the attacker received no message from the guardian" "$(received_nothing; echo $?)"
    rm -f /tmp/f11-arm.$$ /tmp/f11-guard.$$
else
    out=$(attacker squat watchdog 100)
    printf '%s\n' "$out"
    check 4b "the attacker cannot register the watchdog's name: refused by the policy" "$(printf '%s\n' "$out" | grep -q '^STEP name_attach rc -1 '; echo $?)"
    wd_up; arm_up; guard_up
    check 4b "the real watchdog, arm and guardian then start and run" "$(kill -0 "$WD" 2>/dev/null && kill -0 "$ARM" 2>/dev/null && kill -0 "$GUARD" 2>/dev/null; echo $?)"
fi
all_down
if [ "$c" -eq 0 ]; then echo "PASS f11_untrusted_cannot_impersonate_the_watchdog_by_its_name ($MODE)"; else echo "FAIL f11_untrusted_cannot_impersonate_the_watchdog_by_its_name ($MODE)"; fail=1; fi

if [ "$fail" -eq 0 ]; then
    echo "RESULT PASS f11_suite ($MODE)"
else
    echo "RESULT FAIL f11_suite ($MODE)"
fi
exit "$fail"
