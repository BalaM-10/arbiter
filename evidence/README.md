# Evidence

Saved output of Arbiter running on QNX, and of the host checks. Every number in
the documentation comes from a file here (CLAUDE.md rule 6). Nothing here is
built from QNX-licensed files: no binaries, no images.

| Folder | What |
| --- | --- |
| `runs/<date>-<target>/run-<time>.log` | `scripts/run-tests.sh`: the build (each binary's SHA-256) and every on-target test |
| `runs/<date>-<target>/repeat-<test>-<time>.log` | one test repeated (`ONLY=<test> REPEAT=<n>`), every run's start state and results |
| `runs/<date>-<target>/planner-e2e-<time>.log` | `arbiter planner e2e`: the scripted planner through the gateway, with the target's evidence log |
| `runs/<date>-host/` | `scripts/host-checks.sh`, `arbiter evidence verify-packet`, probes |
| `bench/<date>-<target>/<time>/` | `arbiter bench run`: `report.html`, `report.json`, the evidence packet `packet.json`, and `raw/` (the target's records, its evidence logs and system logs) |
| `secpol/` | the security policy draft and its inspection output |
| `stall/` | the QEMU host-pause probe |

## What is kept

Kept: every run a document cites (README, `docs/`), every run that failed or
showed a fault, and the logs saved on purpose before a fix (tests first). Runs
that passed and that no document cites are not kept (2026-11-17, owner request:
17 such bench folders were removed; they stay in the git history).

Kept although no other document cites them:

| Run | Why |
| --- | --- |
| `bench/2026-11-17-qemu_secpol/140207/` | its `packet.json` names the policy as `none` on the policy image: the bug fixed in a8a5151 (the fixed run is `140617`) |
| `bench/2026-11-17-qemu_secpol/150104/` | the policy image at f39c0c0, 9/9, the only bench run of F2, F13, F14, F15 before F7, F8 and F10 were added |
