# Trusted code checklist

Every change to the trusted C code goes through this checklist before it is
committed. The trusted code is `qnx/guardian`, `qnx/arm`, `qnx/watchdog`,
`qnx/libcontract`, the evidence logger `qnx/logd`, and the shared headers and
helpers in `qnx/common` (T1 in the trust list of `docs/THREAT_MODEL.md`). Each item is either checked by a
script (named in the right column) or by reading the change. A failed item is
fixed, or justified in the commit message.

This is a safety-oriented design checklist written for this project. It is not
a coding standard from a certification scheme, and following it does not make
the code certified.

## Build

| Item | How it is checked |
| --- | --- |
| Compiles with `-std=c11 -Wall -Wextra -Werror` for x86_64 and aarch64le | `qnx/Makefile`; every `scripts/run-tests.sh` run builds both architectures from clean and logs the SHA-256 of each binary |
| No compiler-specific extensions without a comment explaining why | Reading the change |

## Memory

| Item | How it is checked |
| --- | --- |
| No `malloc`, `calloc`, `realloc`, `free`, `strdup` or variable-length arrays after initialization | Reading the change (grep for each name); `libcontract` and the arm's motion model also by `scripts/check-pure.sh`, which fails on any library call outside a short list of memory and math functions (NFR-K2) |
| All buffers are fixed-size, with the size as a named constant | Reading the change |
| Every array index and `memcpy` length is bounds-checked against that constant | Reading the change; `make -C qnx analyze analyze-qnx` (gcc `-fanalyzer`, cppcheck) |
| No pointer arithmetic on received data without a prior length check | Reading the change; the malformed-input tests below |

## Control flow

| Item | How it is checked |
| --- | --- |
| No recursion | Reading the change |
| Every loop has a compile-time upper bound stated in a comment | Reading the change |
| No blocking call on the decision path other than the message receive; every `MsgSend()` bounded by `TimerTimeout()` | `scripts/check-bounded-sends.py` (FR-G9) |
| No `printf`, file I/O or formatted logging on the decision path; logging through slog2 after the reply | Reading the change (NFR-Q6) |
| No write that can block (stdout, stderr, files) once the process has registered with the watchdog: every startup line is printed and flushed before the registration, and from then on the process logs through slog2 only | `nfr_q6_a_blocked_stdout_never_trips_the_watchdog_guardian` and `_arm` in `qnx/tests/arm_suite.sh` (stdout a FIFO that is already full); reading the change |
| Only QNX native IPC: message passing for commands and setpoints, pulses for ticks and heartbeats | `scripts/check-ipc.py` (NFR-Q2) |

## Input validation (NFR-I6)

| Item | How it is checked |
| --- | --- |
| Received byte count checked against the expected struct size first | Reading the change; malformed-input tests |
| Version field checked | Reading the change; tests |
| Every numeric field range-checked; floating-point inputs checked for NaN and infinity before any comparison | Reading the change; `libcontract` host and on-target unit tests |
| Enumerated fields checked against the known set, with a default reject | Reading the change |
| Sequence numbers handle wrap-around explicitly | Reading the change; tests |

## Arithmetic

| Item | How it is checked |
| --- | --- |
| No signed overflow; widen before multiplying | Reading the change; host tests built with AddressSanitizer and UndefinedBehaviorSanitizer |
| No division without a non-zero check | Reading the change; static analysis |
| Comparisons that decide safety fail closed: when unsure, deny | Reading the change; boundary-value tests |

## Fail-safe behaviour

| Item | How it is checked |
| --- | --- |
| Every error path leaves the arm in a safe state | Reading the change; the fault bench (F1 to F15) |
| Every return value from a QNX call is checked | Reading the change; static analysis |
| Unknown message types and unknown pulse codes are rejected and counted | Reading the change; tests. Known exception: the guardian's dispatch layer drops unknown pulse codes without counting them (`docs/THREAT_MODEL.md`, known gaps) |
| The default branch of every safety `switch` denies | Reading the change |

## Concurrency

| Item | How it is checked |
| --- | --- |
| State shared between threads has one documented owner or a documented lock | Reading the change |
| Nothing shared across processes uses a mutex or semaphore not declared process-shared (required in QNX 8.0) | Reading the change |
| Each ring buffer has exactly one writer and one reader | Reading the change |

## QNX usage

| Item | How it is checked |
| --- | --- |
| Every QNX call has its QNX SDP 8.0 documentation URL in a comment, and the call is listed on that page | Reading the change; `docs/QNX_EVIDENCE.md` lists each feature with its page |
| No API removed or changed in QNX 8.0 (for example `InterruptAttach()`, `ClockPeriod()` at runtime, `int rcvid`, unregistered events delivered by another process) | Reading the change, against the 8.0 discontinuation notice (https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.discontinuation_notice/topic/kernel.html) and migration guide (https://qnx.com/developers/docs/8.0/com.qnx.doc.qnxsdp.migration/topic/kernel.html) |
| Priorities come from `qnx/common/arb_prio.h`, pulse codes from `qnx/common/arb_pulses.h` | Reading the change |
| Privileges are dropped after startup, keeping only what the process needs | `nfr_i5_*` tests on the target (NFR-I5) |

## Tests

| Item | How it is checked |
| --- | --- |
| Each changed requirement has a test naming its ID | `scripts/check-traceability.py` against `docs/TRACEABILITY.md` |
| `libcontract` changes have host unit tests including boundary values: exactly at a limit, just inside, just outside | Reading the tests |
| Malformed-input tests cover zero length, one byte short, one byte long, maximum length and random bytes | Reading the tests (NFR-I6) |
| Tests pass on the QNX target, not only on the host | `scripts/run-tests.sh qemu`; the log is saved under `evidence/runs/` |
| A fix comes with a test that failed on the target before the fix, and that failing log is kept | `evidence/runs/` (failing logs are kept, see `evidence/README.md`) |

## Static checks

`./scripts/host-checks.sh` runs, among others: gcc `-fanalyzer` and cppcheck over
`libcontract` (`make -C qnx analyze`), qcc's gcc 12 `-fanalyzer` with
`-Wconversion -Wshadow` and cppcheck over the guardian, arm and watchdog
(`make -C qnx analyze-qnx`), the purity check for both architectures, the
bounded-send and native-IPC source checks, and the host unit tests with
AddressSanitizer and UndefinedBehaviorSanitizer.
