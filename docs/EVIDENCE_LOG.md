# Evidence log format (FR-E1, FR-E2, FR-E4, FR-E5, FR-C6)

`arb-logd` reads the guardian's decision ring (`qnx/common/arb_decisions.h`) and
writes one text file per run. `arbiter evidence verify` checks it on the host
with Python's `hashlib`, independently of the code that wrote it.

## Lines

One record per line, ASCII, fields separated by one space, ending in `\n`:

```
<tag> <n> <key> <value> ... prev <64 hex> hash <64 hex>
```

- `<n>` is the line number in the chain, from 0, increasing by exactly 1.
- `prev` is the `hash` of line `n - 1`; on line 0 it is 64 zeros.
- `hash` is the SHA-256, in lowercase hex, of the line's text up to and
  including the `prev` value (everything before ` hash `).

| Tag | Meaning | Keys, in this order |
| --- | --- | --- |
| `H` | Header, line 0 only | `format 1`, `contract <sha256 of the contract table file>` (FR-C6), `cycles_per_sec <n>`, `start_ns <n>` |
| `R` | One decision (FR-E1) | `idx <ring index>`, `t_ns <n>`, `seq <n>`, `target <q0> <q1>`, `dur <ms>`, `verdict <n>`, `rule <n>`, `before <state>`, `after <state>`, `flags <n>`, `fwd <q0> <q1> <ms>`, `latency_cycles <n>` |
| `G` | Records the logger lost (FR-E5): it fell behind and the ring overwrote them, or a copy was torn | `lost <count>`, `from_idx <first lost>`, `to_idx <last lost>` |
| `E` | Clean end (the logger was stopped with SIGTERM) | `records <R lines>`, `lost <total lost>` |

Units: angles in microradians, durations in ms, times in ns of the guardian's
`CLOCK_MONOTONIC`, latency in `ClockCycles()` cycles (divide by
`cycles_per_sec`). Verdict, rule and state numbers are those of
`qnx/libcontract/arb_contract.h`.

## What `arbiter evidence verify` checks

It fails, naming the first bad line, when:

- a line does not parse, or its `n` is not the previous `n + 1`;
- its `prev` is not the previous line's `hash`, or its `hash` does not match its text
  (an edit, a removed line or a reordered line);
- an `R` line's `idx` is not the previous `idx + 1`, unless a `G` line between
  them declares exactly the missing ones (an undeclared gap);
- line 0 is not an `H` line, or an `H` line appears later;
- `--anchor <hex>` is given and the last line's `hash` differs from it (lines
  removed from the end).

Without an anchor, removing lines from the end cannot be detected (TM-C6): the logger
publishes the latest hash (`<log>.anchor`, and in the system log at exit) so it
can be recorded elsewhere (TM-C6). A log with no `E` line is reported as
unterminated (the logger was killed), which is not by itself a failure.
