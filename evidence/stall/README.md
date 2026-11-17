# QEMU host pauses (midpoint review M3)

What a stall of the whole QEMU guest does to Arbiter, and how often it happens on
its own. QEMU proves function only; these times are QEMU measurements on one
laptop, not claims about the Raspberry Pi 5 or any other hardware.

## How it was run

Host: Windows 11 laptop, WSL2, Ubuntu 22.04.5, QEMU 6.2 with KVM, guest with
`-smp 2` (two virtual CPUs). Target: the policy-off image (`qemu`). Tools:
`qnx/tests/arb_stall_probe.c` (on the target, priority 10, as root) and
`scripts/qemu-stall.sh` (on the host). Every log header names the commit.

- **Pauses** (`2026-11-16-qemu/210332-pause.log`, commit 3382c82):
  `./scripts/qemu-stall.sh pause 5 20 50 100 200`. For each value: start the
  watchdog, arm and guardian; the probe moves the arm every 150 ms through the
  guardian (as the gateway's user) and prints every change of the supervision
  counters; about 3 s in, the host stops the QEMU process (`kill -STOP`), sleeps
  that long and continues it (`kill -CONT`); the `HOST` line gives the pause as
  measured on the host. Then `arb-ctl status`, the operator's reset
  (`on -u 800 arb-ctl reset`, from a copy in `/tmp`), `arb-ctl status` again, and
  2 s of moves to check that commands are allowed again.
- **Rate** (`2026-11-16-qemu/210454-rate.log`, commit 3382c82):
  `./scripts/qemu-stall.sh rate 600`. Ten minutes of moves with nothing paused;
  every arm tick interval is recorded (the `SUMMARY` line counts those of 2, 5,
  10, 20 and 50 ms or more); after any `SAFE_STOP` the probe would do the
  operator's reset itself, so later stalls would be counted too.
- **Bench report under a pause** (`2026-11-16-qemu/bench-paused/`, commit
  d798b9a): `arbiter bench run --target qemu --scenarios F1` with the QEMU process
  paused for 50 ms about 9 s after the start, by hand from the host shell. It
  fails on purpose; it shows that the report names who tripped and why, and that
  `raw/F1.slog2.txt` holds the scenario's system log.

## What it shows

- Pauses of 20, 50, 100 and 200 ms (25 to 208 ms measured on the host): each
  stopped the system. The watchdog tripped both clients as stuck at once (it
  measures time since the last heartbeat, and inside the guest everything was
  silent), the arm braked and latched, the guardian entered `SAFE_STOP` (cause
  watchdog, or arm timeout from 50 ms up, when its bounded send to the arm failed
  first). About 10 ms later each client also reported the watchdog lost, because
  a tripped client gets no more beats. Each was recovered by the operator's
  reset: state `HOLD`, then 14 of 14 moves allowed.
- The 5 ms pause (9 ms measured on the host; longest arm tick interval 8.7 ms)
  stopped nothing; 40 of 40 moves were allowed, and the reset that follows in the
  script was refused because there was nothing to reset (`resets_refused 1`).
- No pause produced a burst of queued ticks: after each pause the arm's ticks
  were about 1 ms apart again (no interval under 0.1 ms in any of the five
  runs). The review's hypothesis, that a burst of late ticks makes a
  client think the watchdog is gone, did not occur.
- Ten minutes with nothing paused: 599 930 tick intervals, none of 5 ms or more
  (longest 3.582 ms), no trip, 4000 of 4000 moves allowed. On that run, on an
  otherwise idle host, a stall long enough to trip did not happen.
- It does happen under load: bench F1 on the policy image failed once on
  2026-11-16 (`evidence/bench/2026-11-16-qemu_secpol/203653`) with one trip of
  the guardian only, which fits a pause of one of the two virtual CPUs. That is
  inferred, not reproduced (a signal pauses the whole QEMU process).

## Limits

- The arm's telemetry sees stalls of the virtual CPU the arm runs on. A pause of
  only the other CPU shows up as a watchdog trip, not as a long tick interval.
- One ten-minute run is a sample, not a rate.
