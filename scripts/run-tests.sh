#!/bin/bash
# Deploy, then run every on-target test and save all output under
# evidence/runs/<date>-<target>/. Exits non-zero if any test fails.
#   ./scripts/run-tests.sh qemu
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"

# Each entry: <binary on target>  <what it covers>
TESTS=(
    "hello:M0 toolchain: qcc-built binary runs"
    "msgpass:M0 message passing (M0-MSG-1..7, previews NFR-Q2)"
)

target_init "${1:-}"
"$(dirname "$0")/deploy.sh" "$T_NAME"

STAMP_DATE="$(date +%F)"
STAMP_TIME="$(date +%H%M%S)"
OUTDIR="$REPO_ROOT/evidence/runs/${STAMP_DATE}-${T_NAME}"
LOG="$OUTDIR/run-${STAMP_TIME}.log"
mkdir -p "$OUTDIR"

{
    echo "# run-tests $T_NAME — $(date -Is)"
    dirty="$(git -C "$REPO_ROOT" status --porcelain -- qnx scripts)"
    echo "# commit: $(git -C "$REPO_ROOT" rev-parse --short HEAD)${dirty:+ (UNCOMMITTED changes in qnx/ or scripts/)}"
    echo "# host: $(lsb_release -ds 2>/dev/null || uname -sr); qcc $(qcc -V 2>&1 | awk '/default/{print $1}')"
    echo "# target: $T_USER@$T_HOST:$T_DIR ($T_ARCH)"
    echo "\$ uname -a"
    "${SSH[@]}" uname -a
} | tee "$LOG"

pass=0
fail=0
for entry in "${TESTS[@]}"; do
    bin="${entry%%:*}"
    what="${entry#*:}"
    {
        echo
        echo "## $bin — $what"
        echo "\$ $T_DIR/$bin"
    } | tee -a "$LOG"
    set +e
    "${SSH[@]}" "$T_DIR/$bin" 2>&1 | tee -a "$LOG"
    rc=${PIPESTATUS[0]}
    set -e
    echo "exit=$rc" | tee -a "$LOG"
    if [ "$rc" -eq 0 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
done

total=$((pass + fail))
echo | tee -a "$LOG"
echo "SUMMARY $pass/$total passed on $T_NAME" | tee -a "$LOG"
echo "log: ${LOG#"$REPO_ROOT"/}"
[ "$fail" -eq 0 ]
