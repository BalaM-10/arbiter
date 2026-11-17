# Shared helpers for the on-target suites. Source it, do not run it.

# The security-policy image has secpolpush in its boot image (mkqnximage
# --secpol); there every Arbiter process starts as its own type.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/o/on.html
if [ -e /proc/boot/secpolpush ]; then
    ARB_POLICY_IMAGE=1
    ARB_ON_WD="on -T arb_watchdog_t"
    ARB_ON_ARM="on -T arb_arm_t"
    ARB_ON_GUARD="on -T arb_guardian_t"
    ARB_ON_LOGD="on -T arb_logd_t"
else
    ARB_POLICY_IMAGE=0
    ARB_ON_WD=
    ARB_ON_ARM=
    ARB_ON_GUARD=
    ARB_ON_LOGD=
fi

# The pid of the newest process with this name (a process started through on
# or a shell function is not always $!).
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/p/pidin.html
pid_of() { pidin -p "$1" -F "%a" 2>/dev/null | tail -n +2 | tail -1 | tr -d ' '; }

# NFR-I5 for one trusted process: its own user and group, FIFO at its priority,
# and its startup check line in slog2. Sets i5p / i5d to 1 on a failure.
#   i5_check <name> <slog2 set> <pid> <uid> <prio> <expected drop line>
i5_check() {
    if pidin -p "$1" -F "%a %U %V %b %p" | grep -q "^ *$3  *$4  *$4  *1  *${5}f"; then
        echo "  ok   $1: pid $3 user $4 group $4, FIFO $5"
    else
        echo "  FAIL $1: expected pid $3 user $4 group $4, FIFO $5"
        i5p=1
    fi
    if slog2info -b "$2" | grep "$2.$3 " | grep -qF "$6"; then
        echo "  ok   $1: $6"
    else
        echo "  FAIL $1: no slog2 line '$6'"
        i5d=1
    fi
}
I5_WD_LINE="privileges dropped: uid 812 gid 812; kept PRIORITY 1-66; refused as expected: setuid(0), priority 67, fork()"
I5_ARM_LINE="privileges dropped: uid 811 gid 811; kept PRIORITY 1-70, CONNECTION, XPROCESS_QUERY 812-812; refused as expected: setuid(0), priority 71, fork()"
I5_GUARD_LINE="privileges dropped: uid 810 gid 810; kept PRIORITY 1-64, CONNECTION, XPROCESS_QUERY 811-812; refused as expected: setuid(0), priority 65, fork()"
