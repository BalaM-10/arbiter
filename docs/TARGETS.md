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
| SDP | QNX SDP 8.0, base package `com.qnx.qnx800` version `8.0.0.00141T202311271501L` (Build 141), installed at `~/qnx800` |
| Target architectures | `x86_64`, `aarch64le` |
| Compiler | `qcc` with GCC 12.2.0: variants `gcc_ntox86_64`, `gcc_ntoaarch64le` (and `_gpp`) |
| Image tool | `mkqnximage` (`~/qnx800/host/common/bin/mkqnximage`) |
| Security policy tools in this install | `secpolcompile` (host and target), `secpolpush`, `secpol`, `secpolgenerate` (target) |
| Tracing and logging in this install | `tracelogger`, `traceprinter`, `slogger2`, `slog2info` |
| Not in this install | High Availability Manager (`ham`, `hamctrl`) |

## Every shell

```sh
source ~/qnx800/qnxsdp-env.sh
which qcc            # ~/qnx800/host/linux/x86_64/usr/bin/qcc
echo $QNX_TARGET     # ~/qnx800/target/qnx
```

## Toolchain smoke test (2026-11-12)

```sh
qcc -Vgcc_ntox86_64    -std=c11 -Wall -Wextra -Werror -O2 -o hello-x86_64    hello.c
qcc -Vgcc_ntoaarch64le -std=c11 -Wall -Wextra -Werror -O2 -o hello-aarch64le hello.c
```

Both produced 64-bit ELF executables with interpreter `/usr/lib/ldqnx-64.so.2`
(x86-64 and ARM aarch64). Not yet run on a target.

## QEMU target

Not built yet.
