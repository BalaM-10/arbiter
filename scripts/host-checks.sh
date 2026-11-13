#!/bin/bash
# Host-side checks, saved under evidence/runs/<date>-host/:
#   - libcontract unit tests built with gcc, AddressSanitizer and UBSan (NFR-K2)
#   - static analysis: gcc -fanalyzer and cppcheck over libcontract
#   - purity: libcontract objects (host gcc and qcc, both architectures) call
#     nothing outside the allowed C library set (NFR-K2)
#   - traceability: every requirement has a row; every named test exists
#   - claims: every "cannot" claim in the docs cites docs/THREAT_MODEL.md
#   - Python host tools: pytest, ruff, mypy --strict (NFR-K4)
#   ./scripts/host-checks.sh
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"
sdp_env
cd "$REPO_ROOT"   # mypy and pytest read pyproject.toml paths relative to the repo root

OUTDIR="$REPO_ROOT/evidence/runs/$(date +%F)-host"
LOG="$OUTDIR/host-checks-$(date +%H%M%S).log"
mkdir -p "$OUTDIR"

fail=0
step() {   # step <title> <command...>
    local title="$1"; shift
    echo; echo "## $title"; echo "\$ $*"
    if "$@" 2>&1 | grep -v -E '^make(\[[0-9]+\])?: (Entering|Leaving)'; [ "${PIPESTATUS[0]}" -eq 0 ]; then
        echo "-> ok"
    else
        echo "-> FAILED"; fail=$((fail + 1))
    fi
}

{
    dirty="$(git -C "$REPO_ROOT" status --porcelain -- qnx scripts docs/TRACEABILITY.md)"
    echo "# host-checks — $(date -Is)"
    echo "# commit: $(git -C "$REPO_ROOT" rev-parse --short HEAD)${dirty:+ (UNCOMMITTED changes)}"
    echo "# host: $(lsb_release -ds 2>/dev/null || uname -sr); $(gcc --version | head -1); $(cppcheck --version)"
    step "libcontract unit tests (host, ASan + UBSan)" make -C "$REPO_ROOT/qnx" -B host-test
    step "static analysis (gcc -fanalyzer, cppcheck)"   make -C "$REPO_ROOT/qnx" analyze
    step "purity, x86_64"                                make -C "$REPO_ROOT/qnx" check-pure ARCH=x86_64
    step "purity, aarch64le"                             make -C "$REPO_ROOT/qnx" check-pure ARCH=aarch64le
    step "traceability"                                  python3 -I "$REPO_ROOT/scripts/check-traceability.py"
    step "threat-model references"                       python3 -I "$REPO_ROOT/scripts/check-claims.py"
    step "python tests (pytest)"                         "$REPO_ROOT/.venv/bin/python" -m pytest -q "$REPO_ROOT/host/tests"
    step "nfr_k4_python_lint_and_types: ruff"            "$REPO_ROOT/.venv/bin/ruff" check "$REPO_ROOT/host"
    step "nfr_k4_python_lint_and_types: mypy --strict"   "$REPO_ROOT/.venv/bin/mypy" --config-file "$REPO_ROOT/pyproject.toml"
    echo
    echo "SUMMARY host-checks: $([ "$fail" -eq 0 ] && echo PASS || echo "FAIL ($fail step(s))")"
} 2>&1 | tee "$LOG"

echo "log: ${LOG#"$REPO_ROOT"/}"
grep -q '^SUMMARY host-checks: PASS' "$LOG"
