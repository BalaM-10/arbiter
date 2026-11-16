#!/bin/bash
# Build Arbiter's QEMU images with mkqnximage (QNX-licensed output: built
# locally under the git-ignored targets/, never committed).
#
#   ./scripts/build-image.sh qemu                 policy off  -> targets/qemu-x86_64
#   ./scripts/build-image.sh qemu_secpol          policy on   -> targets/qemu-x86_64-secpol
#   ./scripts/build-image.sh qemu_secpol develop  the recording image used to draft
#                                                 the policy with secpolgenerate
#
# The policy image compiles qnx/image/policy/arbiter.secpol into the boot image
# (/proc/boot/secpol.bin) and pushes it at boot, before any resource manager
# starts (FR-S4). Only one QEMU image runs at a time: this script stops the
# image it rebuilds; stop the other one yourself (mkqnximage --stop in its
# directory). Needs no root.
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html
# (options --secpol and --policy: mkqnximage --help=secpol, --help=policy)
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"

NAME="${1:-}"
MODE="${2:-}"
sdp_env
. "$REPO_ROOT/scripts/targets.env"

KEY_PUB="${TARGET_KEY}.pub"
[ -r "$KEY_PUB" ] || die "SSH public key $KEY_PUB not readable (see docs/TARGETS.md)"
POLICY="$REPO_ROOT/qnx/image/policy/arbiter.secpol"

case "$NAME:$MODE" in
    qemu:)
        DIR="$REPO_ROOT/$qemu_IMAGE_DIR"
        OPTS=(--hostname=arbiter-qemu --secpol=no --policy=none)
        ;;
    qemu_secpol:)
        DIR="$REPO_ROOT/$qemu_secpol_IMAGE_DIR"
        # Check the policy compiles before building anything.
        secpolcompile "$POLICY" || die "the policy does not compile: $POLICY"
        OPTS=(--hostname=arbiter-secpol --secpol=secure "--policy=$POLICY")
        ;;
    qemu_secpol:develop)
        DIR="$REPO_ROOT/$qemu_secpol_IMAGE_DIR"
        OPTS=(--hostname=arbiter-secpol --secpol=develop --policy=none)
        ;;
    *)
        die "usage: $0 qemu | qemu_secpol [develop]"
        ;;
esac

mkdir -p "$DIR"
cd "$DIR"
mkqnximage --stop >/dev/null 2>&1 || true
echo "\$ (cd ${DIR#"$REPO_ROOT"/} && mkqnximage --type=qemu --arch=x86_64 ${OPTS[*]//$REPO_ROOT\//} --ssh-ident=<key>.pub --build)"
mkqnximage --type=qemu --arch=x86_64 "${OPTS[@]}" "--ssh-ident=$KEY_PUB" --build
echo "built ${DIR#"$REPO_ROOT"/}; start it with: (cd ${DIR#"$REPO_ROOT"/} && mkqnximage --run=-h)"
