#!/bin/bash
# What a stall of the whole QEMU guest does to Arbiter (midpoint review M3).
# Policy-off QEMU image only (the probe runs as root and uses `on -u`).
#
#   ./scripts/qemu-stall.sh pause <ms> [<ms> ...]
#       For each duration: start the watchdog, arm and guardian; run
#       `arb-stall-probe watch 6` (the arm moving); about 3 s in, pause the whole
#       QEMU process on the host (SIGSTOP, sleep, SIGCONT); then show the
#       guardian's status, recover with the operator's `arb-ctl reset`, show the
#       status again and check that moves are allowed again (`watch 2`).
#   ./scripts/qemu-stall.sh rate <seconds>
#       Start the system once and run `arb-stall-probe rate <seconds>` with
#       nothing paused: how often the host stalls the guest on its own. The probe
#       resets after each SAFE_STOP, so every stall in the run is counted.
#
# Output: evidence/stall/<date>-qemu/<time>-<mode>.log, with the commit in its
# header. How to read it: evidence/stall/README.md.
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"

MODE="${1:-}"
shift || true
case "$MODE" in
    pause) [ "$#" -ge 1 ] || die "usage: $0 pause <ms> [<ms> ...]" ;;
    rate)  [ "$#" -eq 1 ] || die "usage: $0 rate <seconds>" ;;
    *)     die "usage: $0 pause <ms> [<ms> ...] | rate <seconds>" ;;
esac
for v in "$@"; do
    case "$v" in ''|*[!0-9]*) die "not a whole number: $v" ;; esac
done

target_init qemu
[ "$T_POLICY" = off ] || die "the stall probe runs on the policy-off image only"
PIDFILE="$REPO_ROOT/targets/qemu-x86_64/output/qemu.pid"
[ -r "$PIDFILE" ] || die "no $PIDFILE: is the policy-off QEMU image running?"
QPID="$(cat "$PIDFILE")"
kill -0 "$QPID" 2>/dev/null || die "QEMU pid $QPID from $PIDFILE is not running"

OUTDIR="$REPO_ROOT/evidence/stall/$(date +%F)-qemu"
LOG="$OUTDIR/$(date +%H%M%S)-$MODE.log"
mkdir -p "$OUTDIR"

"$REPO_ROOT/.venv/bin/arbiter" contract corpus "$REPO_ROOT/contracts/default.yaml" \
    -o "$REPO_ROOT/build/contract-corpus" >/dev/null
"$(dirname "$0")/deploy.sh" qemu >/dev/null

UP='cd '"$T_DIR"' && slay -f arb-guardian arb-arm arb-watchdog >/dev/null 2>&1; sleep 1;
(./arb-watchdog </dev/null >/dev/null 2>&1 &); sleep 1;
(./arb-arm contract-corpus/contract.bin </dev/null >/dev/null 2>&1 &); sleep 1;
(./arb-guardian contract-corpus/contract.bin 800 </dev/null >/dev/null 2>&1 &); sleep 1'
DOWN='slay -f arb-guardian arb-arm arb-watchdog >/dev/null 2>&1; true'
# The operator console as the operator (user 800 cannot enter root's home; same as arm_suite.sh).
RESET='cp ./arb-ctl /tmp/arb-ctl.stall && chmod 755 /tmp/arb-ctl.stall && on -u 800 /tmp/arb-ctl.stall reset; rm -f /tmp/arb-ctl.stall'

{
    dirty="$(git -C "$REPO_ROOT" status --porcelain -- qnx scripts)"
    echo "# qemu-stall $MODE $* — $(date -Is)"
    echo "# commit: $(git -C "$REPO_ROOT" rev-parse --short HEAD)${dirty:+ (UNCOMMITTED changes in qnx/ or scripts/)}"
    echo "# host: $(lsb_release -ds 2>/dev/null || uname -sr); QEMU pid $QPID: $(tr '\0' ' ' </proc/"$QPID"/cmdline | grep -o -- '-smp [0-9]*' || echo 'smp ?')"
    echo "# target: $T_USER@$T_HOST:$T_DIR, $("${SSH[@]}" uname -a)"
    echo "# QEMU proves function; the times are QEMU measurements, not claims."
} | tee "$LOG"

if [ "$MODE" = rate ]; then
    echo "## rate $1 s: nothing paused" | tee -a "$LOG"
    "${SSH[@]}" "$UP; ./arb-stall-probe rate $1; ./arb-ctl status; $DOWN" 2>&1 | tee -a "$LOG"
    echo "log: ${LOG#"$REPO_ROOT"/}"
    exit 0
fi

for ms in "$@"; do
    echo "## pause $ms ms" | tee -a "$LOG"
    part="$(mktemp)"
    "${SSH[@]}" "$UP; ./arb-stall-probe watch 6; echo '## status after the pause'; ./arb-ctl status;
        echo '## operator reset (arb-ctl reset as user 800)'; $RESET;
        echo '## moves after the reset'; ./arb-stall-probe watch 2; $DOWN" >"$part" 2>&1 &
    ssh_pid=$!
    for _ in $(seq 1 200); do                       # bound: 200 x 0.1 s for the probe to start
        grep -q '^START' "$part" && break
        sleep 0.1
    done
    grep -q '^START' "$part" || { kill "$ssh_pid" 2>/dev/null; cat "$part" | tee -a "$LOG"; rm -f "$part"; die "the probe did not start"; }
    sleep 3
    t0=$(date +%s%N)
    kill -STOP "$QPID"
    sleep "$(awk -v m="$ms" 'BEGIN { printf "%.3f", m / 1000 }')"
    kill -CONT "$QPID"
    t1=$(date +%s%N)
    wait "$ssh_pid" || true
    echo "HOST paused the QEMU process for $(( (t1 - t0) / 1000000 )) ms (SIGSTOP to SIGCONT), about 3 s into the watch" | tee -a "$LOG"
    tee -a "$LOG" <"$part"
    rm -f "$part"
done
echo "log: ${LOG#"$REPO_ROOT"/}"
