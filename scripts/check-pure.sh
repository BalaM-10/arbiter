#!/bin/bash
# NFR-K2: libcontract must make no OS call. List the undefined symbols of a
# compiled libcontract object and fail on anything outside the allowed set of
# C standard library functions (memory and math only).
#   scripts/check-pure.sh <nm-tool> <object.o>
set -euo pipefail

NM="${1:?usage: $0 <nm-tool> <object.o>}"
OBJ="${2:?usage: $0 <nm-tool> <object.o>}"

# memcpy/memset: <string.h>; sin/cos/fabs: <math.h>; sincos: the same math,
# emitted by gcc when it merges a sin() and cos() of the same angle.
# cexp: <complex.h>; the QNX 8.0 gcc merges the same sin()/cos() pair into it.
# __stack_chk_fail, __stack_chk_guard: the compiler's stack protector (on by
# default in both compilers); the function runs only if the stack is corrupted.
# _GLOBAL_OFFSET_TABLE_: linker table for position-independent code (qcc builds
# PIE); a data symbol, not a call.
ALLOWED='^(memcpy|memset|sin|cos|sincos|cexp|fabs|__stack_chk_fail|__stack_chk_guard|_GLOBAL_OFFSET_TABLE_)$'

undefined="$("$NM" -u "$OBJ" | awk '{print $NF}' | sort -u)"
bad="$(printf '%s\n' "$undefined" | grep -v -E "$ALLOWED" | grep -v '^$' || true)"

echo "check-pure $OBJ: external symbols: $(printf '%s ' $undefined)"
if [ -n "$bad" ]; then
    echo "FAIL nfr_k2_libcontract_is_pure: not allowed: $(printf '%s ' $bad)" >&2
    exit 1
fi
echo "PASS nfr_k2_libcontract_is_pure"
