# Targets and toolchain

The exact host, SDP and target versions this project was built and tested with.
The README's build steps must match this file.

## Development host

| Item | Value |
| --- | --- |
| Machine | Windows 11 laptop, Intel Core i7-10850H |
| Linux | Ubuntu 22.04.5 LTS under WSL2 (distro `Ubuntu-22.04`), kernel 6.18.58.1-microsoft-standard-WSL2, systemd enabled |
| Why this host | Ubuntu Desktop 22.04 LTS is a supported SDP 8.0 development host ([SDP 8.0 release notes](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.release_notes/topic/sdp8_rn.html)); QEMU is the documented virtualization for Ubuntu hosts ([mkqnximage](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html)) |
| QEMU | 6.2.0 (`qemu-system-x86` 1:6.2+dfsg-2ubuntu6.31, `qemu-utils`), `bridge-utils` 1.7-1ubuntu3 |
| KVM | `/dev/kvm` available in WSL2; user in group `kvm`. `mkqnximage`'s QEMU run script uses `--enable-kvm --cpu host` when `/dev/kvm` is writable |
| Host C compiler | gcc 11.4.0, GNU Make 4.3 (`build-essential`), for host unit tests of `libcontract` |

## QNX software

| Item | Value |
| --- | --- |
| QNX Software Center | 2.0.4, installed at `~/qnx/qnxsoftwarecenter/` |
| SDP | **QNX SDP 8.0.4**: base `com.qnx.qnx800` 8.0.0.00141T202311271501L (Build 141) at `~/qnx800`, updated in place on 2026-11-12 with patch set `com.qnx.qnx800.patchset804` 8.0.4.00384T202603311511L from the Software Center's Available tab (log: "Installing Software is done: OK") |
| How to tell the level on disk | The patch set is not listed under `~/qnx800/.packages/metadata` and the base package keeps its 8.0.0 version string. Key packages after the patch: `target.microkernel.core` 2.4.0.00111T202602271127L, `host...qcc` 8.0.0.00121T202510081309L, `host...gcc` 12.2.0.00301T202506091801L, `host.mkqnximage` 0.3.0.00600T202507302003L. `procnto-smp-instr` (x86_64) carries build stamp `2026/02/27-11:02:56EST`. The target's `uname` reports `8.0.0` for every 8.0.x level |
| Why 8.0.4 | 8.0.4 fixes an aarch64 delay/hang when sending or receiving messages or pulses (ref 3007752) and several kernel priority-inheritance issues; 8.0.3 added `mkqnximage` images for physical boards such as the Pi ([8.0.4 notes](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.release_notes/topic/8.0.4_prod_release_notes/8.0.4_rn.html), [8.0.3 notes](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.release_notes/topic/8.0.3_prod_release_notes/8.0.3_rn.html)) |
| Target architectures | `x86_64`, `aarch64le` |
| Compiler | `qcc` with GCC 12.2.0 (`QNX8.0.0-rc240b01...-102`): variants `gcc_ntox86_64`, `gcc_ntoaarch64le` (and `_gpp`) |
| Image tool | `mkqnximage` (`~/qnx800/host/common/bin/mkqnximage`); QEMU networking is a host bridge `br0` (172.31.1.1/24) with `dnsmasq` DHCP, set up by its `net.sh` via sudo on first run. Host needs `ifconfig` (`net-tools`) and `dnsmasq` (`dnsmasq-base`) |
| Security policy tools in this install | `secpolcompile` (host and target), `secpolpush`, `secpol`, `secpolgenerate` (target) |
| Tracing and logging in this install | `tracelogger`, `traceprinter`, `slogger2`, `slog2info` |
| Not in this install | High Availability Manager (`ham`, `hamctrl`) |

## Every shell

```sh
source ~/qnx800/qnxsdp-env.sh
which qcc            # ~/qnx800/host/linux/x86_64/usr/bin/qcc
echo $QNX_TARGET     # ~/qnx800/target/qnx
```

## Toolchain smoke test (2026-11-12, re-run after the 8.0.4 patch set)

```sh
qcc -Vgcc_ntox86_64    -std=c11 -Wall -Wextra -Werror -O2 -o hello-x86_64    hello.c
qcc -Vgcc_ntoaarch64le -std=c11 -Wall -Wextra -Werror -O2 -o hello-aarch64le hello.c
```

Both produced 64-bit ELF executables with interpreter `/usr/lib/ldqnx-64.so.2`
(x86-64 and ARM aarch64). Not yet run on a target.

## QEMU target

Not built yet.
