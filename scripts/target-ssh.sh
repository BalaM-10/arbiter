#!/bin/bash
# Run a command on a named target, or copy a file to it, with the settings in
# scripts/targets.env. Used by `arbiter bench run` so the bench and the test
# scripts reach the target the same way.
#   ./scripts/target-ssh.sh qemu info                 print "<arch> <deploy dir>"
#   ./scripts/target-ssh.sh qemu run '<command>'      stdin is passed through
#   ./scripts/target-ssh.sh qemu put <local> <remote path relative to the deploy dir>
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"

target_init "${1:-}"
case "${2:-}" in
    info) echo "$T_ARCH $T_DIR" ;;
    run)  exec "${SSH[@]}" "cd '$T_DIR' && ${3:?command}" ;;
    put)  "${SSH[@]}" "mkdir -p '$T_DIR/$(dirname "${4:?remote}")'"
          exec "${SCP[@]}" "${3:?local}" "$T_USER@$T_HOST:$T_DIR/$4" ;;
    *)    die "usage: $0 <target> info|run <cmd>|put <local> <remote>" ;;
esac
