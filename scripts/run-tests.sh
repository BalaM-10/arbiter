#!/bin/bash
# Build for every architecture, deploy, run every on-target test, and save all
# output under evidence/runs/<date>-<target>/. Exits non-zero if any test fails.
#   ./scripts/run-tests.sh qemu
#   REPEAT=50 ./scripts/run-tests.sh qemu    # run each test 50 times
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"

# Each entry: <command run in the target directory>:<what it covers>
TESTS=(
    "hello:M0 toolchain: qcc-built binary runs"
    "msgpass:M0 message passing (M0-MSG-1..7, previews NFR-Q2)"
    "test_libcontract:libcontract unit tests on QNX (FR-C4, FR-C5, FR-G2..G5, FR-G9 logic, NFR-I6, NFR-K2)"
    "test_contract_file contract-corpus/manifest.txt:compiled contract loads on QNX exactly as written; corrupted tables rejected (FR-C3, FR-C4)"
    "test_send_timeout:2 ms TimerTimeout on MsgSend, SEND and REPLY states, measured (FR-G9 design check)"
)
ALL_ARCHS=(x86_64 aarch64le)

REPEAT="${REPEAT:-1}"
case "$REPEAT" in
    ''|*[!0-9]*|0) die "REPEAT must be a positive integer, got '$REPEAT'" ;;
esac

target_init "${1:-}"

STAMP_DATE="$(date +%F)"
STAMP_TIME="$(date +%H%M%S)"
OUTDIR="$REPO_ROOT/evidence/runs/${STAMP_DATE}-${T_NAME}"
LOG="$OUTDIR/run-${STAMP_TIME}.log"
mkdir -p "$OUTDIR"

{
    dirty="$(git -C "$REPO_ROOT" status --porcelain -- qnx scripts)"
    echo "# run-tests $T_NAME — $(date -Is), REPEAT=$REPEAT"
    echo "# commit: $(git -C "$REPO_ROOT" rev-parse --short HEAD)${dirty:+ (UNCOMMITTED changes in qnx/ or scripts/)}"
    echo "# host: $(lsb_release -ds 2>/dev/null || uname -sr); qcc $(qcc -V 2>&1 | awk '/default/{print $1}')"
    echo
    echo "## contract corpus (host compiler)"
    echo "\$ arbiter contract corpus contracts/default.yaml -o build/contract-corpus"
    "$REPO_ROOT/.venv/bin/arbiter" contract corpus "$REPO_ROOT/contracts/default.yaml" \
        -o "$REPO_ROOT/build/contract-corpus" || die "contract corpus failed (is .venv set up? see docs/TARGETS.md)"
    sha256sum "$REPO_ROOT/build/contract-corpus/contract.bin" | sed "s|$REPO_ROOT/||; s|^|  |"
    echo
    echo "## build (all architectures)"
    for a in "${ALL_ARCHS[@]}"; do
        echo "\$ make -C qnx ARCH=$a"
        mk_out="$(make -C "$REPO_ROOT/qnx" ARCH="$a" 2>&1)" || { printf '%s\n' "$mk_out"; die "build failed for $a"; }
        printf '%s\n' "$mk_out" | grep -v -E '^make(\[[0-9]+\])?: (Entering|Leaving)' || true
        for f in $(find "$REPO_ROOT/build/$a" -maxdepth 1 -type f -perm -u+x | sort); do
            echo "  $(basename "$f")  sha256 $(sha256sum "$f" | cut -d' ' -f1)"
            echo "    $(file -b "$f" | cut -d, -f1-2)"
        done
    done
} 2>&1 | tee "$LOG"
[ "${PIPESTATUS[0]}" -eq 0 ] || exit 1

"$(dirname "$0")/deploy.sh" "$T_NAME" | tee -a "$LOG"

{
    echo
    echo "## target: $T_USER@$T_HOST:$T_DIR ($T_ARCH)"
    echo "\$ uname -a"
    "${SSH[@]}" uname -a
} | tee -a "$LOG"

pass=0
fail=0
for entry in "${TESTS[@]}"; do
    bin="${entry%%:*}"
    what="${entry#*:}"
    {
        echo
        echo "## $bin — $what"
        echo "\$ cd $T_DIR && ./$bin"
    } | tee -a "$LOG"

    ok=0
    for ((i = 1; i <= REPEAT; i++)); do
        set +e
        out="$("${SSH[@]}" "cd '$T_DIR' && ./$bin" 2>&1)"
        rc=$?
        set -e
        # Full output for the first run and for every failing run.
        if [ "$i" -eq 1 ] || [ "$rc" -ne 0 ]; then
            [ "$i" -eq 1 ] || echo "--- run $i of $REPEAT failed:" | tee -a "$LOG"
            printf '%s\n' "$out" | tee -a "$LOG"
            echo "exit=$rc" | tee -a "$LOG"
        fi
        [ "$rc" -eq 0 ] && ok=$((ok + 1))
    done
    [ "$REPEAT" -eq 1 ] || echo "repeat: $ok/$REPEAT runs passed" | tee -a "$LOG"

    if [ "$ok" -eq "$REPEAT" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
done

total=$((pass + fail))
echo | tee -a "$LOG"
echo "SUMMARY $pass/$total tests passed on $T_NAME (each run $REPEAT time(s))" | tee -a "$LOG"
echo "log: ${LOG#"$REPO_ROOT"/}"
[ "$fail" -eq 0 ]
