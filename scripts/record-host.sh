#!/bin/bash
# Record the development host, the installed QNX SDP level and the QEMU
# networking state as evidence, so docs/TARGETS.md quotes saved output.
#   ./scripts/record-host.sh        -> evidence/host/<date>-host.txt
# Reads only version strings, package directory names and the procnto build
# stamp. It copies no SDP file and never reads the QNX license, myQNX account
# data or Software Center logs; a final check refuses to write the record if
# anything resembling them appears.
set -euo pipefail
. "$(dirname "$0")/lib/target.sh"
sdp_env

SDP="${QNX_BASE:-$HOME/qnx800}"
META="$SDP/.packages/metadata"
OUT="$REPO_ROOT/evidence/host/$(date +%F)-host.txt"
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
mkdir -p "$(dirname "$OUT")"

pkg_version() {   # installed version of one SDP package, from its metadata directory name
    ls "$META/$1" 2>/dev/null | head -1
}

{
    echo "# host record — $(date -Is), commit $(git -C "$REPO_ROOT" rev-parse --short HEAD)"
    echo
    echo "## host"
    echo "os: $(lsb_release -ds)"
    echo "kernel: $(uname -r)"
    echo "wsl distro: ${WSL_DISTRO_NAME:-not WSL}"
    echo "qemu: $(qemu-system-x86_64 --version | head -1)"
    echo "dnsmasq: $(dnsmasq --version | head -1)"
    echo "host gcc: $(gcc --version | head -1)"
    echo "kvm: $( [ -w /dev/kvm ] && echo '/dev/kvm writable' || echo '/dev/kvm NOT writable')"
    echo
    echo "## QNX SDP at ${SDP/#$HOME/~}"
    echo "qcc variants:"
    qcc -V 2>&1 | sed -n 's/^[[:space:]]*\(12\.[^ ]*\).*/  \1/p'
    for p in com.qnx.qnx800 \
             com.qnx.qnx800.target.microkernel.core \
             com.qnx.qnx800.host.linux.x86_64.qcc \
             com.qnx.qnx800.host.linux.x86_64.gcc \
             com.qnx.qnx800.host.mkqnximage \
             com.qnx.qnx800.target.security.secpol.tools; do
        echo "package $p: $(pkg_version "$p")"
    done
    echo "procnto-smp-instr (x86_64) build stamp: $(strings -a "$QNX_TARGET/x86_64/boot/sys/procnto-smp-instr" | grep -m1 -E '^[0-9]{4}/[0-9]{2}/[0-9]{2}-')"
    echo
    echo "## QEMU networking (scripts/qemu-br0-dhcp.sh)"
    echo "br0: $(ip -br addr show dev br0 2>/dev/null || echo 'absent')"
    echo "dnsmasq on br0: $(pgrep -af 'dnsmasq.*interface=br0' || echo 'not running')"
    echo "UDP sockets on ports 53 and 67 (dnsmasq should hold only br0:67; port 53 belongs to WSL DNS and systemd-resolved):"
    ss -lunp 2>/dev/null | awk 'NR==1 || /:67 |:53 /' | sed 's/^/  /'
    echo "leases (expiry mac ip ...):"
    sed 's/^/  /' /var/lib/misc/dnsmasq.br0.leases 2>/dev/null || echo "  none"
} > "$TMP"

# Guard: no license key, serial number, e-mail address, account or Software
# Center log detail may end up in the record.
if grep -n -i -E '@|licen[cs]e|serial|myqnx|swupdate|qnxsoftwarecenter|cookie|session|password|token|[A-Z0-9]{4}(-[A-Z0-9]{4}){3,}' "$TMP"; then
    echo "error: the lines above look like account or license data; record not written" >&2
    exit 1
fi
mv "$TMP" "$OUT"
trap - EXIT

echo "wrote ${OUT#"$REPO_ROOT"/}"
cat "$OUT"
