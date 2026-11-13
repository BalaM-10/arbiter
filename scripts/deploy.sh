#!/bin/bash
# Build the QNX binaries for a target and copy them to it over SSH.
#   ./scripts/deploy.sh qemu
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"

target_init "${1:-}"

make -C "$REPO_ROOT/qnx" ARCH="$T_ARCH" >/dev/null
BUILD="$REPO_ROOT/build/$T_ARCH"

"${SSH[@]}" "mkdir -p '$T_DIR'"
"${SCP[@]}" "$BUILD"/* "$T_USER@$T_HOST:$T_DIR/"
echo "deployed $(cd "$BUILD" && ls | tr '\n' ' ')to $T_NAME ($T_USER@$T_HOST:$T_DIR)"
