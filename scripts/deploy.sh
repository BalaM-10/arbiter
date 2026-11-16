#!/bin/bash
# Build the QNX binaries for a target and copy them to it over SSH.
#   ./scripts/deploy.sh qemu
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"

target_init "${1:-}"

# A clean build, so the binaries come from the current sources whatever the
# files' modification times. run-tests.sh builds (and logs) first, then sets
# ARB_PREBUILT=1 so that exactly the binaries it logged are deployed.
if [ "${ARB_PREBUILT:-0}" != 1 ]; then
    make -C "$REPO_ROOT/qnx" ARCH="$T_ARCH" clean all >/dev/null
fi
BUILD="$REPO_ROOT/build/$T_ARCH"

"${SSH[@]}" "mkdir -p '$T_DIR'"
# Executables only (build/<arch>/obj holds intermediate objects).
mapfile -t FILES < <(find "$BUILD" -maxdepth 1 -type f -perm -u+x | sort)
[ "${#FILES[@]}" -gt 0 ] || die "nothing to deploy in $BUILD"
"${SCP[@]}" "${FILES[@]}" "$T_USER@$T_HOST:$T_DIR/"
# Contract test corpus (architecture-independent), written by `arbiter contract corpus`.
CORPUS="$REPO_ROOT/build/contract-corpus"
if [ -d "$CORPUS" ]; then
    "${SSH[@]}" "rm -rf '$T_DIR/contract-corpus'"
    "${SCP[@]}" -r "$CORPUS" "$T_USER@$T_HOST:$T_DIR/contract-corpus"
fi
echo "deployed $(for f in "${FILES[@]}"; do printf '%s ' "$(basename "$f")"; done)to $T_NAME ($T_USER@$T_HOST:$T_DIR)"
