# Targets and toolchain

The exact host, SDP and target versions this project was built and tested with,
and the steps to reproduce the setup. The README's quick start must match this
file. Versions below are recorded by `scripts/record-host.sh` in
[`evidence/host/2026-11-13-host.txt`](../evidence/host/2026-11-13-host.txt).

## Development host

| Item | Value |
| --- | --- |
| Machine | Windows 11 laptop, Intel Core i7-10850H |
| Linux | Ubuntu 22.04.5 LTS under WSL2 (distro `Ubuntu-22.04`), kernel 6.18.58.1-microsoft-standard-WSL2, systemd enabled |
| Why this host | Ubuntu Desktop 22.04 LTS is a supported SDP 8.0 development host ([SDP 8.0 release notes](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.release_notes/topic/sdp8_rn.html)); QEMU is the documented virtualization for Ubuntu hosts ([mkqnximage](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnximage.html)) |
| QEMU | 6.2.0 (`qemu-system-x86` 1:6.2+dfsg-2ubuntu6.31, `qemu-utils`), `bridge-utils` 1.7-1ubuntu3 |
| KVM | `/dev/kvm` available in WSL2; user in group `kvm`. `mkqnximage`'s QEMU run script uses `--enable-kvm --cpu host` when `/dev/kvm` is writable |
| Host C compiler | gcc 11.4.0, GNU Make 4.3 (`build-essential`), for host-side unit tests (from M1) |
| DHCP for the QEMU bridge | dnsmasq 2.91 (`dnsmasq-base`); `net-tools` provides `arp`/`ifconfig` used by `mkqnximage` |

## QNX software

| Item | Value |
| --- | --- |
| QNX Software Center | 2.0.4, installed at `~/qnx/qnxsoftwarecenter/` |
| SDP | **QNX SDP 8.0.4**: base `com.qnx.qnx800` 8.0.0.00141T202311271501L (Build 141) at `~/qnx800`, updated in place on 2026-11-12 with patch set `com.qnx.qnx800.patchset804` 8.0.4.00384T202603311511L from the Software Center's Available tab |
| How to tell the level on disk | The patch set is not listed under `~/qnx800/.packages/metadata`, and the base package keeps its 8.0.0 version string. Look instead at the key packages (`target.microkernel.core` 2.4.0.00111T202602271127L, `host...qcc` 8.0.0.00121T202510081309L, `host...gcc` 12.2.0.00301T202506091801L, `host.mkqnximage` 0.3.0.00600T202507302003L) and the `procnto-smp-instr` build stamp `2026/02/27-11:02:56EST`. The target's `uname` reports `8.0.0` for every 8.0.x level. All recorded in `evidence/host/2026-11-13-host.txt` |
| Why 8.0.4 | 8.0.4 fixes an aarch64 delay/hang when sending or receiving messages or pulses (ref 3007752) and several kernel priority-inheritance issues; 8.0.3 added `mkqnximage` images for physical boards such as the Pi ([8.0.4 notes](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.release_notes/topic/8.0.4_prod_release_notes/8.0.4_rn.html), [8.0.3 notes](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.release_notes/topic/8.0.3_prod_release_notes/8.0.3_rn.html)) |
| Target architectures | `x86_64` (QEMU), `aarch64le` (Raspberry Pi 5, from M3) |
| Compiler | `qcc` with GCC 12.2.0: variants `gcc_ntox86_64`, `gcc_ntoaarch64le` |
| Image tool | `mkqnximage` (`~/qnx800/host/common/bin/mkqnximage`) |
| Security policy tools in this install | `secpolcompile` (host and target), `secpolpush`, `secpol`, `secpolgenerate` (target) |
| Tracing and logging in this install | `tracelogger`, `traceprinter`, `slogger2`, `slog2info` |
| Not in this install | High Availability Manager (`ham`, `hamctrl`) |

## First-time setup (Ubuntu 22.04, native or WSL2)

Do these once, in order, from the repo root. Steps 3 to 5 need a myQNX account.

1. **Host packages.**
   ```sh
   sudo apt update
   sudo apt install -y qemu-system-x86 qemu-utils bridge-utils build-essential net-tools dnsmasq-base
   ```
2. **KVM access** (QEMU is much faster with it). Join the `kvm` group, then
   restart the shell; on WSL2, run `wsl --terminate Ubuntu-22.04` in PowerShell
   and reopen Ubuntu.
   ```sh
   sudo usermod -aG kvm $USER
   test -w /dev/kvm && echo "KVM OK"      # after the restart
   ```
3. **License.** Get the free non-commercial QNX SDP 8.0 license at
   <https://www.qnx.com/getqnx> and deploy it in your myQNX account.
4. **QNX Software Center.** From myQNX, download the **Linux hosts** installer
   (`qnx-setup-<build>-lin.run`), check its checksum against the download page,
   and run it **without sudo**:
   ```sh
   chmod a+x qnx-setup-*-lin.run && ./qnx-setup-*-lin.run
   ```
5. **SDP 8.0.4.** In the Software Center: **Add Installation → QNX Software
   Development Platform 8.0**, install to `~/qnx800` with the `x86_64` and
   `aarch64le` targets. Then, for that installation, open **Available → QNX
   Software Development Platform – Minor Release → Patchset** and install the
   8.0.4 patch set.
6. **SSH key for the targets** (no passphrase, so the test scripts run unattended;
   it only unlocks your own local targets):
   ```sh
   mkdir -p ~/.ssh && chmod 700 ~/.ssh
   ssh-keygen -t ed25519 -N "" -C "arbiter-qnx-target" -f ~/.ssh/qnx_target_ed25519
   ```
7. **Build the QEMU image** (QNX-licensed, so it stays in the git-ignored `targets/`):
   ```sh
   source ~/qnx800/qnxsdp-env.sh
   mkdir -p targets/qemu-x86_64 && cd targets/qemu-x86_64
   mkqnximage --type=qemu --arch=x86_64 --hostname=arbiter-qemu \
     --ssh-ident=$HOME/.ssh/qnx_target_ed25519.pub --build
   cd ../..
   ```
8. **Python host tools** (contract compiler and its tests), in a project-local
   virtual environment; versions are pinned in `pyproject.toml`, licenses in `NOTICE`:
   ```sh
   sudo apt install -y python3-venv cppcheck
   python3 -m venv .venv
   .venv/bin/pip install -e ".[dev]"
   ```

## Every session

```sh
source ~/qnx800/qnxsdp-env.sh                  # puts qcc and mkqnximage on PATH
sudo ./scripts/qemu-br0-dhcp.sh                # once per boot (WSL restart); see below
(cd targets/qemu-x86_64 && mkqnximage --run=-h && mkqnximage --getip)   # boot QEMU in the background
./scripts/run-tests.sh qemu                    # build, deploy, run, save the log
(cd targets/qemu-x86_64 && mkqnximage --stop)  # when finished
```

`run-tests.sh` builds both architectures, records each binary's SHA-256, copies
the x86_64 binaries to the target over SSH, prints the target's `uname -a`, runs
every test, and saves the whole output under `evidence/runs/<date>-qemu/`.
`REPEAT=N ./scripts/run-tests.sh qemu` runs each test N times.
The guest's address comes from DHCP and can change between boots; the scripts
always look it up with `mkqnximage --getip`.

## The two QEMU images: policy off and policy on (M2 P5, D5)

| Target name | Directory | Built with | Security policy | What runs there |
| --- | --- | --- | --- | --- |
| `qemu` | `targets/qemu-x86_64` | `./scripts/build-image.sh qemu` | none (`--secpol=no`) | every on-target test and the bench: the code-level checks alone (FR-S6) |
| `qemu_secpol` | `targets/qemu-x86_64-secpol` | `./scripts/build-image.sh qemu_secpol` | `qnx/image/policy/arbiter.secpol`, enforced (`--secpol=secure`) | `policy_suite.sh`, `f11_suite.sh on` and the bench, every Arbiter process as its own type |

Only one QEMU image runs at a time (they share the `br0` bridge):

```sh
(cd targets/qemu-x86_64 && mkqnximage --stop)
./scripts/build-image.sh qemu_secpol               # checks the policy compiles, then builds (no root needed)
(cd targets/qemu-x86_64-secpol && mkqnximage --run=-h)
./scripts/run-tests.sh qemu_secpol
./.venv/bin/arbiter bench run --target qemu_secpol
(cd targets/qemu-x86_64-secpol && mkqnximage --stop)
```

On `qemu_secpol` the SSH root login runs as type `user_root_t`. The policy lets it
start the Arbiter types and the attacker's type and use `/dev/arbiter` like any
client, but not connect to the arm or the watchdog (owner decision 2026-11-15),
so the tests that play the guardian or the watchdog (`test_arm`, `test_watchdog`,
`guardian_suite.sh`, `test_e2e wdforged` and `log`) run on `qemu` only.

To redraft the policy: `./scripts/build-image.sh qemu_secpol develop` builds the
same image with `secpolgenerate` recording what runs (nothing is refused in that
mode); exercise the system, copy `/dev/secpolgenerate/policy` off the target
with `ssh ... cat` (the docs say not scp), then edit by hand as the header of
`arbiter.secpol` describes, and rebuild with `./scripts/build-image.sh qemu_secpol`.

## QEMU networking: `scripts/qemu-br0-dhcp.sh`

**How `mkqnximage --run` networks the guest.** Through a host bridge `br0`
(172.31.1.1/24), QEMU's setuid `qemu-bridge-helper`, and a `dnsmasq` DHCP
server, all set up by the SDP's `host/common/mkqnximage/qemu/net.sh`, which runs:

```sh
dnsmasq --interface=br0 --bind-interfaces --except-interface=lo0 --dhcp-range=172.31.1.2,172.31.1.254
```

**The problem on Linux, visible under WSL2.** dnsmasq adds the loopback
interface itself whenever `--interface` is given (dnsmasq(8), `--interface`).
The Linux loopback is `lo`, not `lo0`, so the exclusion has no effect and
dnsmasq tries to listen for DNS on every loopback address. WSL2's DNS tunnel
already holds port 53 on `10.255.255.254` (on `lo`), so dnsmasq exits with
`failed to create listening socket for 10.255.255.254: Address already in use`.
The guest gets no lease, and `mkqnximage --getip` reports `Failed to find address`.

**How `--getip` finds the guest.** It does not read DHCP leases. It reads the
host ARP table (`arp -an`) for the guest's MAC (`OPT_MACADDR` in
`local/options`) and pings each match. The image's `pinger.sh`, run by the
guest's DHCP hook, pings the host to put the guest in that table. Without a
lease there is no entry.

**The fix.** `scripts/qemu-br0-dhcp.sh` performs the same setup as `net.sh`
(allow `br0` in `/etc/qemu/bridge.conf`, make `qemu-bridge-helper` setuid root,
create `br0` at 172.31.1.1/24) and starts dnsmasq with two changes:

- `--except-interface=lo`: the Linux loopback name, which is what `net.sh` intended.
- `--port=0`: no DNS at all, DHCP only (dnsmasq(8), `--port`). `br0` has no NAT
  to the outside, so the guest does not need a DNS server here.

The SDP is not modified. `mkqnximage`'s own check (`qemu/check-net`) then finds
everything in place and does not run `net.sh`. Leases are written to
`/var/lib/misc/dnsmasq.br0.leases`. To undo:
`sudo kill "$(cat /run/dnsmasq-br0.pid)"; sudo ip link del br0`.

Recorded state with the fix in place (dnsmasq on `br0:67` only, the guest's
lease): `evidence/host/2026-11-13-host.txt`. Target output (`uname -a` over
SSH): the header of every run log in `evidence/runs/`.
