# Design addendum for M2: keep-out on links, and the guardian's front door

Status: **approved by the owner 2026-11-16**, with the exit rule tightened to
"strictly shallower" (section 1). Responds to the midpoint review of 2026-11-15,
findings B1 and Major 1. Section 1 is implemented; section 2 is next.

## 1. Keep-out on both links (B1)

### What changes

Today `path_keepout()` tests two points of the arm, the elbow and the tip. It will
test the two links as line segments: the upper arm (base to elbow) and the forearm
(elbow to tip), at the same 129 samples of the joint-space path, against each zone
inflated by the same margin.

**Depth of a link in a zone.** For a point, depth is how far it is inside the
inflated rectangle (the smallest of its distances to the four sides; > 0 inside).
For a link, depth is the largest depth of any point on it. Along the segment each
of the four distances is a straight-line function of the position, so the largest
"smallest of four" lies at an end of the segment or where two of the four lines
cross: at most 2 + 6 = 8 candidate points, computed exactly. A link is inside a
zone when its depth is > 0. Pure, bounded (129 samples x 4 zones x 2 links x 8
candidates), no new dependency.

**The margin argument still holds.** Between two samples no point of either link
moves further than the tip does (D = (L1 + L2)·s1 + L2·s2), so the swept link lies
within D/2 of a sampled link. Inflating each zone by D/2 (plus the start tolerance
and 1 µm) keeps the sampled test conservative, as before.

### Exit-only rule (owner decision 2026-11-16)

A move whose start pose has a link inside an inflated zone is allowed only if all
of these hold; otherwise it is DENY, rule KEEPOUT, counted as a violation:

1. **Each link gets strictly shallower.** At every sample, for every zone, each
   link still inside that zone is less deep than at the previous sample.
2. **Nothing new enters.** A link that was outside a zone at the previous sample
   is still outside it (this also covers crossing a second zone on the way out).
3. **The final pose is fully clear.** At the last sample both links are outside
   every inflated zone.

Allowed exit moves keep the flag `ARB_VFLAG_KEEPOUT_EXIT` and do not count as
violations. A move that starts clear is checked by rule 2 alone (any entry denies),
which is today's behaviour applied to links.

Decided:

- **Per link, not the whole arm.** "Shallower" is judged for the upper arm and the
  forearm separately; judging only the deeper of the two would let one link go
  deeper while the other stays the deepest.
- **Strictly shallower, not "not deeper"** (owner's choice). The two differ only
  at exactly equal depths, which a continuous path almost never produces, so no
  test isolates the difference. Known limit: a link can still slide sideways
  inside a zone while its deepest point gets shallower. Exit moves exist only for
  recovery (the arm stopped inside, or a new contract adds a zone around it) and
  are flagged in every verdict.
- **Zones around the base.** The upper arm always touches the base, so a zone
  there could never be left. The contract compiler refuses a zone within 20 mm of
  the base.
- **Links are lines.** Real links have width; the contract must add their
  half-width to each zone.

### Tests (written first)

- Committed first and seen failing on QEMU (`evidence/runs/2026-11-16-qemu/run-091932.log`,
  test_libcontract 64/67): the review's 25°/25° case, an upper arm crossing a
  small zone, a forearm sweeping through a zone mid-move (libcontract); the bench's
  own judge on forearm and upper arm, exact segment clearance, F5 denying the B1
  pose (pytest).
- Added with the rule: a forearm starting across the zone that swings clear
  (allowed, flagged); one that goes deeper first (denied); one that turns back in
  on the way out (denied; removing the depth rule makes only this test fail); one
  that ends still across (denied); the upper arm leaving through a second zone
  (denied); the compiler refusing a zone at the base.
- The four existing exit tests pass unchanged under the link rule. Three margin
  tests had the forearm pointing into the zone; they now bend it away, so they
  still test only the elbow's clearance.

### Wording fixed

`contracts/default.yaml` comment, ARCHITECTURE "Path checking", "Leaving a
keep-out zone" and "What is not covered", F5 pass condition, DESIGN_M1_ADDENDUM
bench section, TRACEABILITY: "neither the elbow nor the tip" became "no part of
either link".

F5 also changed: its "beside the zone" pose (shoulder 27°, elbow 0°) had the
forearm across the hand area, so it moved to shoulder 18°, which passes below the
area's corner at about 19 mm, and F5 gained a denied B1 command.

## 2. The guardian's front door (Major 1)

### Who may do what on `/dev/arbiter`

| Request | Who | Checked how |
| --- | --- | --- |
| open read-only | any process that may connect (policy image: listed types only) | file mode |
| open for writing | the gateway user (814) and the operator (800) | file mode 0664, owner 814, group with 814 and 800 |
| `SUBMIT` (a command) | **the gateway user only**, on its one submitter open | kernel-reported euid of the sender (`ConnectClientInfo()`, as `is_operator()` does today), on every devctl |
| `GET_STATUS` | any open | |
| `ESTOP` | any open (D2, confirmed 2026-11-16: stopping is the safe direction) | |
| `RESET` | the operator only, never through the gateway | existing check |

The file mode is a first layer; the euid check at each `SUBMIT` is the one that
counts, so it does not depend on the mode or on startup order. On the policy image
a new type `arb_gateway_t` may connect only to the guardian (FR-S2, second half),
added in P6 when the gateway binary exists.

**One submitter at a time.** The first write open by the gateway user takes the
submitter slot. A second write open by the gateway user while the slot is held is
refused with `EBUSY` at open. The slot is freed when that open closes for any
reason, including the process dying (the kernel closes it).

### "The gateway died", with several clients

- **Gateway dies or closes:** its submitter open closes, and `NORMAL` goes to
  `HOLD` at once, whatever else holds the device open (logger, dashboard relay,
  `arb-ctl`). This replaces today's "last open of any kind".
- **Gateway hangs (alive, silent):** the existing 250 ms silence timeout moves
  `NORMAL` to `HOLD`. Unchanged.
- **Other clients die:** no state change.
- Test: a status reader holds the device open while the submitter is killed; the
  guardian is in `HOLD` within one tick and the arm stops on its segment.

### How a restarted planner learns the sequence number

Proposed: **keep one sequence space for the guardian's lifetime** (as today,
replay protection survives everything) **and publish it**:

- `arb_status_t` gains `last_seq` and a flag "has a sequence" (status layout
  version bump; 48 to 56 bytes).
- The gateway reads the status when a planner connects and sends it a hello frame
  with `last_seq`; the planner starts at `last_seq + 1`. A restarted gateway does
  the same before accepting a planner.
- Rejected alternative: a fresh sequence space whenever the gateway reconnects. It
  is simpler for the planner, but a command captured in one session could then be
  replayed in the next.

### Tests (written first, before any gateway code)

- `fr_g10_only_the_gateway_user_may_submit`: root, the operator, the untrusted user
  get `EPERM`; the gateway user gets a verdict.
- `fr_g10_a_second_submitter_is_refused`: `EBUSY` while the first is open; allowed
  after it closes.
- `fr_g5_submitter_killed_with_a_status_reader_open_holds_at_once`.
- `fr_g10_status_reports_the_last_sequence`, and a restarted client that resumes
  from it gets `ALLOW`.
- `fr_g5_reset_through_the_gateway_is_refused` (promised in ARCHITECTURE).
- F11 case 5: the untrusted user submits a command: `EPERM` with the policy off,
  refused by the kernel with the policy on.

Test tools that submit today run as root; they will run as the gateway user.

### Requirement IDs

New **FR-G10** (approved 2026-11-16): "Only the gateway's user may submit commands, on one
submitter connection at a time; the status publishes the last sequence number."
FR-G5's client-loss sentence changes from "the last client" to "the submitter".
