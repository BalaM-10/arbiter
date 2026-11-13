#!/bin/bash
# Set up the br0 bridge and DHCP that `mkqnximage --run` expects for a QEMU
# target, with dnsmasq bound to br0 only.
#
# Why this exists (WSL2): mkqnximage's own setup script
# ($QNX_HOST/../common/mkqnximage/qemu/net.sh) starts
#   dnsmasq --interface=br0 --bind-interfaces --except-interface=lo0 \
#           --dhcp-range=172.31.1.2,172.31.1.254
# dnsmasq adds the loopback interface automatically when --interface is given
# (dnsmasq(8), --interface). On Linux the loopback is "lo", not "lo0", so the
# exclusion does nothing and dnsmasq tries to open DNS port 53 on every lo
# address. WSL2 puts its DNS tunnel at 10.255.255.254 on lo, already on
# port 53, so dnsmasq exits with "failed to create listening socket for
# 10.255.255.254: Address already in use" and the guest never gets a lease.
#
# Fix: same bridge, address and DHCP range as net.sh, plus
#   --except-interface=lo   the Linux name of the loopback, as net.sh intended
#   --port=0                no DNS at all, DHCP only (dnsmasq(8), --port).
#                           br0 has no NAT to the outside, so the guest has no
#                           use for a DNS server here.
#
# Run once per WSL boot (and once before the very first `mkqnximage --run`):
#   sudo ./scripts/qemu-br0-dhcp.sh
# mkqnximage's check (qemu/check-net) then finds br0 and a dnsmasq with
# "interface=br0" running and does not start its own.
#
# Undo: sudo kill "$(cat /run/dnsmasq-br0.pid)"; sudo ip link del br0
set -euo pipefail

BR=br0
BR_ADDR=172.31.1.1/24
DHCP_RANGE=172.31.1.2,172.31.1.254
LEASES=/var/lib/misc/dnsmasq.br0.leases
PIDFILE=/run/dnsmasq-br0.pid
BRIDGE_CONF=/etc/qemu/bridge.conf
BRIDGE_HELPER=/usr/lib/qemu/qemu-bridge-helper

if [ "$(id -u)" -ne 0 ]; then
    echo "error: run as root: sudo $0" >&2
    exit 1
fi

# qemu-bridge-helper must be allowed to use br0 and be setuid root, so QEMU
# (run as you) can attach the guest to br0. These are the same two steps
# mkqnximage's net.sh performs; doing them here means no failing first run.
if [ ! -x "$BRIDGE_HELPER" ]; then
    echo "error: ${BRIDGE_HELPER} not found; install QEMU: sudo apt install qemu-system-x86" >&2
    exit 1
fi
if ! grep -qs "^allow ${BR}\$" "$BRIDGE_CONF"; then
    mkdir -p "$(dirname "$BRIDGE_CONF")"
    echo "allow ${BR}" >> "$BRIDGE_CONF"
    echo "added 'allow ${BR}' to ${BRIDGE_CONF}"
fi
if [ "$(stat -c %u "$BRIDGE_HELPER")" != 0 ]; then
    chown 0 "$BRIDGE_HELPER"
fi
if [ ! -u "$BRIDGE_HELPER" ]; then
    chmod u+s "$BRIDGE_HELPER"
    echo "made ${BRIDGE_HELPER} setuid root"
fi
for tool in dnsmasq ip ss; do
    command -v "$tool" >/dev/null || { echo "error: '$tool' missing; sudo apt install dnsmasq-base iproute2" >&2; exit 1; }
done

# Bridge and host address (same values as net.sh).
if ! ip link show "$BR" >/dev/null 2>&1; then
    ip link add "$BR" type bridge
    echo "created ${BR}"
fi
if ! ip -4 addr show dev "$BR" | grep -q "inet ${BR_ADDR}"; then
    ip addr add "$BR_ADDR" dev "$BR"
fi
ip link set "$BR" up

# DHCP on br0 only.
if pgrep -f 'dnsmasq.*interface=br0' >/dev/null; then
    echo "dnsmasq already running on ${BR}:"
    pgrep -af 'dnsmasq.*interface=br0'
else
    dnsmasq \
        --interface="$BR" --bind-interfaces --except-interface=lo \
        --port=0 \
        --dhcp-range="$DHCP_RANGE" \
        --dhcp-leasefile="$LEASES" \
        --pid-file="$PIDFILE"
    echo "started dnsmasq (pid $(cat "$PIDFILE")), DHCP only on ${BR}"
fi

ip -br addr show dev "$BR"
echo "listening sockets of dnsmasq:"
ss -lunp | grep dnsmasq || true
echo "leases file: ${LEASES}"
