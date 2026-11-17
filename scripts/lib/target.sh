# Shared helpers for deploy.sh and run-tests.sh. Source, do not run.
# Usage: target_init <name>   sets T_NAME T_ARCH T_USER T_DIR T_HOST T_TIMING T_POLICY T_GW_IFACE T_ENV and SSH/SCP arrays.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

die() {
    echo "error: $*" >&2
    exit 1
}

sdp_env() {
    if [ -z "${QNX_TARGET:-}" ]; then
        [ -f "$HOME/qnx800/qnxsdp-env.sh" ] || die "SDP not found at ~/qnx800 (see docs/TARGETS.md)"
        # shellcheck disable=SC1091
        . "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
    fi
}

target_init() {
    T_NAME="${1:-}"
    [ -n "$T_NAME" ] || die "usage: $0 <target>   (targets: qemu, qemu_secpol, pi5)"
    # shellcheck disable=SC1091
    . "$REPO_ROOT/scripts/targets.env"

    local v
    v="${T_NAME}_ARCH"; T_ARCH="${!v:-}"
    [ -n "$T_ARCH" ] || die "unknown target '$T_NAME' (see scripts/targets.env)"
    v="${T_NAME}_USER"; T_USER="${!v}"
    v="${T_NAME}_DIR";  T_DIR="${!v}"
    v="${T_NAME}_SSH_HOSTKEY_OPTS"; local hostkey_opts="${!v}"
    v="${T_NAME}_TIMING"; T_TIMING="${!v:-}"
    case "$T_TIMING" in strict|function) ;; *) die "${T_NAME}_TIMING must be strict or function (scripts/targets.env)" ;; esac
    v="${T_NAME}_GW_IFACE"; T_GW_IFACE="${!v:-}"
    v="${T_NAME}_GW_IFACE_REQUIRED"
    if [ "${!v:-}" = yes ] && [ -z "$T_GW_IFACE" ]; then
        die "${T_NAME}_GW_IFACE is not set: the gateway must be told its wired interface on $T_NAME (scripts/targets.env, docs/TARGETS.md)"
    fi
    case "$T_GW_IFACE" in *[!A-Za-z0-9_]*) die "${T_NAME}_GW_IFACE '$T_GW_IFACE' is not an interface name" ;; esac
    # What the target-side scripts pass to arb-gateway.
    T_ENV="ARB_GW_ARGS='${T_GW_IFACE:+-i $T_GW_IFACE}'"
    v="${T_NAME}_POLICY"; T_POLICY="${!v:-}"
    case "$T_POLICY" in off|on) ;; *) die "${T_NAME}_POLICY must be off or on (scripts/targets.env)" ;; esac

    [ -r "$TARGET_KEY" ] || die "SSH key $TARGET_KEY not readable"

    sdp_env

    T_HOST="${TARGET_HOST:-}"
    if [ -z "$T_HOST" ]; then
        v="${T_NAME}_IMAGE_DIR"; local image_dir="${!v:-}"
        if [ -n "$image_dir" ]; then          # a QEMU image built by mkqnximage
            [ -d "$REPO_ROOT/$image_dir" ] || die "no QEMU image in $image_dir (see docs/TARGETS.md)"
            T_HOST="$(cd "$REPO_ROOT/$image_dir" && mkqnximage --getip 2>/dev/null)" || T_HOST=""
            [ -n "$T_HOST" ] || die "QEMU target not reachable: 'mkqnximage --getip' found no address.
  1. sudo ./scripts/qemu-br0-dhcp.sh        (after every WSL restart)
  2. cd $image_dir && mkqnximage --run=-h   (only one QEMU image at a time)
  3. retry"
        else
            v="${T_NAME}_HOST"; T_HOST="${!v:-}"
            [ -n "$T_HOST" ] || die "no host for '$T_NAME': set ${T_NAME^^}_HOST or TARGET_HOST"
        fi
    fi

    # shellcheck disable=SC2206
    SSH=(ssh -i "$TARGET_KEY" -o BatchMode=yes -o ConnectTimeout=10 $hostkey_opts "$T_USER@$T_HOST")
    # shellcheck disable=SC2206
    SCP=(scp -q -i "$TARGET_KEY" -o BatchMode=yes -o ConnectTimeout=10 $hostkey_opts)

    "${SSH[@]}" true </dev/null 2>/dev/null || die "cannot SSH to $T_USER@$T_HOST (target '$T_NAME')"
}
